#include "maintenance/process_controller.hpp"
#include "maintenance/config_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <curl/curl.h>

#include <thread>
#include <chrono>
#include <iostream>

namespace maintenance {

namespace {

std::wstring quote_arg_windows(const std::wstring& arg) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

} // namespace

ProcessController::ProcessController(std::filesystem::path gateway_exe_path, const gateway::CredentialStore& cred_store)
    : gateway_exe_path_(std::move(gateway_exe_path)), cred_store_(cred_store) {}

ProcessController::~ProcessController() {
    cleanup_handle();
}

void ProcessController::set_active_admin_credential_name(std::string name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!name.empty()) {
        active_admin_credential_name_ = std::move(name);
    }
}

void ProcessController::set_active_port(uint16_t port) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    active_port_ = port;
}

uint16_t ProcessController::active_port() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_port_;
}

void ProcessController::request_cancel() noexcept {
    cancel_requested_.store(true, std::memory_order_release);
}

void ProcessController::reset_cancel() noexcept {
    cancel_requested_.store(false, std::memory_order_release);
}

bool ProcessController::is_cancel_requested() const noexcept {
    return cancel_requested_.load(std::memory_order_acquire);
}

std::string ProcessController::drain_stderr_pipe_locked() {
    if (!stderr_read_pipe_) return "";
    std::string output;
    DWORD avail = 0;
    while (PeekNamedPipe(stderr_read_pipe_, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
        char buf[512];
        DWORD to_read = (avail < sizeof(buf)) ? avail : static_cast<DWORD>(sizeof(buf));
        DWORD read_bytes = 0;
        if (!ReadFile(stderr_read_pipe_, buf, to_read, &read_bytes, nullptr) || read_bytes == 0) {
            break;
        }
        output.append(buf, read_bytes);
        if (output.size() >= 4096) break;
    }
    while (!output.empty() && (output.back() == '\r' || output.back() == '\n' || output.back() == ' ')) {
        output.pop_back();
    }
    return output;
}

std::string ProcessController::describe_process_exit() {
    std::lock_guard<std::mutex> lock(mutex_);
    DWORD exit_code = 0;
    bool have_code = (process_handle_ != nullptr && GetExitCodeProcess(process_handle_, &exit_code) != FALSE);
    std::string stderr_msg = drain_stderr_pipe_locked();
    std::string cfg_str = gateway::path_to_utf8(last_config_path_);

    std::string desc = "Gateway process exited";
    if (have_code) {
        desc += " (exit_code=" + std::to_string(exit_code) + ")";
    }
    if (!cfg_str.empty()) {
        desc += " [config=" + cfg_str + "]";
    }
    if (!stderr_msg.empty()) {
        desc += ": " + stderr_msg;
    } else {
        desc += ".";
    }
    return desc;
}

void ProcessController::cleanup_handle_locked() {
    if (stderr_read_pipe_) {
        CloseHandle(stderr_read_pipe_);
        stderr_read_pipe_ = nullptr;
    }
    if (process_handle_) {
        CloseHandle(process_handle_);
        process_handle_ = nullptr;
    }
    pid_ = 0;
}

void ProcessController::cleanup_handle() {
    std::lock_guard<std::mutex> lock(mutex_);
    cleanup_handle_locked();
}

bool ProcessController::is_running() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!process_handle_) return false;
    DWORD exit_code = 0;
    if (GetExitCodeProcess(process_handle_, &exit_code)) {
        return (exit_code == STILL_ACTIVE);
    }
    return false;
}

DWORD ProcessController::process_id() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pid_;
}

bool ProcessController::is_matching_gateway_process(DWORD target_pid) const {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, target_pid);
    if (!hProc) return false;

    wchar_t path_buf[MAX_PATH];
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, path_buf, &size);
    CloseHandle(hProc);

    if (!ok) return false;

    std::filesystem::path target_path(path_buf);
    std::error_code ec;
    return std::filesystem::equivalent(target_path, gateway_exe_path_, ec);
}

bool ProcessController::start_gateway(const std::filesystem::path& config_path, std::string* error_out) {
    cleanup_handle();

    std::error_code ec;
    if (!std::filesystem::exists(gateway_exe_path_, ec)) {
        if (error_out) *error_out = "Gateway executable not found: " + gateway::path_to_utf8(gateway_exe_path_);
        return false;
    }

    // Try reading port/admin_credential_name from config_path if valid so we track the running instance
    try {
        if (std::filesystem::exists(config_path, ec)) {
            auto cfg = gateway::load_config_from_file(config_path, false);
            std::lock_guard<std::mutex> lock(mutex_);
            active_port_ = cfg.listen.port;
            active_admin_credential_name_ = cfg.security.admin_credential_name;
        }
    } catch (...) {
        // Ignore here; child process will report the exact config error via stderr pipe
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    }

    std::wstring wcmdline = quote_arg_windows(gateway_exe_path_.wstring()) + L" --config " + quote_arg_windows(config_path.wstring());
    std::wstring wexe = gateway_exe_path_.wstring();
    std::wstring wdir = gateway_exe_path_.parent_path().wstring();

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (write_pipe) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe;
        si.hStdError = write_pipe;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }

    PROCESS_INFORMATION pi = {};

    BOOL created = CreateProcessW(
        wexe.c_str(),
        wcmdline.data(),
        nullptr,
        nullptr,
        write_pipe ? TRUE : FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        wdir.c_str(),
        &si,
        &pi
    );

    if (write_pipe) {
        CloseHandle(write_pipe);
        write_pipe = nullptr;
    }

    if (!created) {
        if (read_pipe) CloseHandle(read_pipe);
        if (error_out) *error_out = "CreateProcessW failed with error code: " + std::to_string(GetLastError()) + " [exe=" + gateway::path_to_utf8(gateway_exe_path_) + "]";
        return false;
    }

    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cleanup_handle_locked();
        process_handle_ = pi.hProcess;
        stderr_read_pipe_ = read_pipe;
        pid_ = pi.dwProcessId;
        last_config_path_ = config_path;
    }

    // Wait briefly (100ms) to detect immediate startup failures (invalid config, mutex conflict, AVX2 failure)
    if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
        std::string exit_desc = describe_process_exit();
        cleanup_handle();
        if (error_out) *error_out = exit_desc;
        return false;
    }

    return true;
}

bool ProcessController::stop_gateway(uint16_t port, std::string* error_out, const std::string& admin_credential_name) {
    if (is_cancel_requested()) {
        if (error_out) *error_out = "Stop operation cancelled.";
        return false;
    }

    std::string resolved_admin_cred = admin_credential_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (resolved_admin_cred.empty()) {
            resolved_admin_cred = !active_admin_credential_name_.empty() ? active_admin_credential_name_ : "llm_gateway|admin";
        }
    }

    if (!is_running() && process_id() == 0) {
        reattach_gateway(port);
    }

    // Attempt graceful shutdown via POST /admin/shutdown
    std::string admin_token;
    auto secret_opt = cred_store_.get_token_credential(resolved_admin_cred);
    if (secret_opt) {
        admin_token = std::string(secret_opt->view());
    }

    bool http_sent = false;
    long http_code = 0;
    if (!admin_token.empty()) {
        std::string url = "http://127.0.0.1:" + std::to_string(port) + "/admin/shutdown";
        CURL* curl = curl_easy_init();
        if (curl) {
            curl_slist* headers = nullptr;
            std::string auth = "Authorization: Bearer " + admin_token;
            headers = curl_slist_append(headers, auth.c_str());

            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
            curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 3000L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

            CURLcode res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            http_sent = (res == CURLE_OK && http_code >= 200 && http_code < 300);
        }
        SecureZeroMemory(admin_token.data(), admin_token.size());
    }

    HANDLE wait_handle = nullptr;
    DWORD target_pid = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (process_handle_) {
            DuplicateHandle(GetCurrentProcess(), process_handle_, GetCurrentProcess(), &wait_handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
            target_pid = pid_;
        }
    }

    if (wait_handle) {
        auto wait_start = std::chrono::steady_clock::now();
        while (true) {
            if (is_cancel_requested()) {
                CloseHandle(wait_handle);
                if (error_out) *error_out = "Stop operation cancelled.";
                return false;
            }
            DWORD wait_res = WaitForSingleObject(wait_handle, 100);
            if (wait_res == WAIT_OBJECT_0) {
                CloseHandle(wait_handle);
                cleanup_handle();
                return true;
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count() >= 5000) {
                break;
            }
        }

        // Timeout or HTTP failed: force terminate if not cancelled
        if (is_cancel_requested()) {
            CloseHandle(wait_handle);
            if (error_out) *error_out = "Stop operation cancelled.";
            return false;
        }

        bool terminated = false;
        if (is_matching_gateway_process(target_pid)) {
            BOOL term_ok = TerminateProcess(wait_handle, 0);
            DWORD wait_res = WaitForSingleObject(wait_handle, 2000);
            terminated = (term_ok != FALSE && wait_res == WAIT_OBJECT_0);
        } else {
            DWORD wait_res = WaitForSingleObject(wait_handle, 0);
            terminated = (wait_res == WAIT_OBJECT_0);
        }
        CloseHandle(wait_handle);
        cleanup_handle();

        if (!terminated) {
            if (error_out) *error_out = "Failed to terminate Gateway process (PID=" + std::to_string(target_pid) + ").";
            return false;
        }
        return true;
    }

    // No process handle available; if HTTP shutdown was accepted (2xx), wait up to 5s for port release
    if (http_sent) {
        auto wait_start = std::chrono::steady_clock::now();
        while (health_client_.is_port_open(port)) {
            if (is_cancel_requested()) {
                if (error_out) *error_out = "Stop operation cancelled.";
                return false;
            }
            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start).count() >= 5000) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        cleanup_handle();
        if (health_client_.is_port_open(port)) {
            if (error_out) *error_out = "Gateway accepted /admin/shutdown but port " + std::to_string(port) + " remained open after 5 seconds.";
            return false;
        }
        return true;
    }

    cleanup_handle();
    if (error_out) {
        if (http_code > 0) {
            *error_out = "Failed to stop Gateway via /admin/shutdown (HTTP " + std::to_string(http_code) + " using credential '" + resolved_admin_cred + "') and process handle was unavailable.";
        } else {
            *error_out = "Failed to gracefully stop gateway via HTTP (credential '" + resolved_admin_cred + "') and process handle was unavailable.";
        }
    }
    return false;
}

bool ProcessController::reattach_gateway(uint16_t port) {
    cleanup_handle();

    auto health = health_client_.check_health(port);
    DWORD target_pid = health ? health->pid : 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);

    DWORD found_pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (is_matching_gateway_process(entry.th32ProcessID)) {
                if (target_pid > 0 && entry.th32ProcessID == target_pid) {
                    found_pid = entry.th32ProcessID;
                    break;
                } else if (found_pid == 0) {
                    found_pid = entry.th32ProcessID;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (found_pid > 0) {
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, found_pid);
        if (hProc) {
            std::lock_guard<std::mutex> lock(mutex_);
            cleanup_handle_locked();
            process_handle_ = hProc;
            pid_ = found_pid;
            active_port_ = port;
            return true;
        }
    }
    return false;
}

bool ProcessController::restart_pipeline(
    const gateway::GatewayConfig& new_config,
    const std::filesystem::path& config_path,
    std::function<void(RestartStage, const std::string&)> progress_callback,
    std::string* error_out) {

    auto check_cancelled = [&]() -> bool {
        if (is_cancel_requested()) {
            if (error_out) *error_out = "Restart cancelled.";
            if (progress_callback) progress_callback(RestartStage::Failed, "Restart cancelled.");
            return true;
        }
        return false;
    };

    auto sleep_cancellable = [&](std::chrono::milliseconds total_sleep) -> bool {
        auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < total_sleep) {
            if (check_cancelled()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return !check_cancelled();
    };

    if (check_cancelled()) return false;

    // 1. Validating
    if (progress_callback) progress_callback(RestartStage::Validating, "Validating configuration...");
    try {
        nlohmann::json serialized = gateway::serialize_config(new_config);
        (void)gateway::parse_and_validate_config(serialized, false);
    } catch (const std::exception& e) {
        std::string msg = std::string("Configuration validation failed: ") + e.what();
        if (error_out) *error_out = msg;
        if (progress_callback) progress_callback(RestartStage::Failed, msg);
        return false;
    }
    if (check_cancelled()) return false;

    // Determine currently running port/admin credential before overwriting config on disk
    uint16_t prev_port = new_config.listen.port;
    std::string prev_admin_cred = new_config.security.admin_credential_name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_port_ != 0) prev_port = active_port_;
        if (!active_admin_credential_name_.empty()) prev_admin_cred = active_admin_credential_name_;
    }

    // 2. Saving
    if (progress_callback) progress_callback(RestartStage::Saving, "Saving configuration atomically...");
    ConfigStore store;
    store.set_path(config_path);
    if (!store.save_atomic(new_config, error_out)) {
        if (progress_callback) progress_callback(RestartStage::Failed, error_out && !error_out->empty() ? *error_out : "Failed to save configuration.");
        return false;
    }
    if (check_cancelled()) return false;

    // 3. Stopping
    if (is_running() || health_client_.is_port_open(prev_port) || health_client_.is_port_open(new_config.listen.port)) {
        if (progress_callback) progress_callback(RestartStage::Stopping, "Stopping running Gateway...");
        uint16_t stop_port = (is_running() || health_client_.is_port_open(prev_port)) ? prev_port : new_config.listen.port;
        if (!stop_gateway(stop_port, error_out, prev_admin_cred)) {
            if (progress_callback) progress_callback(RestartStage::Failed, error_out && !error_out->empty() ? *error_out : "Failed to stop running Gateway.");
            return false;
        }
        if (check_cancelled()) return false;
    }

    // 4. WaitingPortRelease
    if (progress_callback) progress_callback(RestartStage::WaitingPortRelease, "Waiting for port release...");
    auto port_wait_start = std::chrono::steady_clock::now();
    while (health_client_.is_port_open(prev_port) || health_client_.is_port_open(new_config.listen.port)) {
        if (check_cancelled()) return false;
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - port_wait_start).count() > 5) {
            uint16_t busy_port = health_client_.is_port_open(new_config.listen.port) ? new_config.listen.port : prev_port;
            if (error_out) *error_out = "Port " + std::to_string(busy_port) + " was not released after 5 seconds.";
            if (progress_callback) progress_callback(RestartStage::Failed, *error_out);
            return false;
        }
        if (!sleep_cancellable(std::chrono::milliseconds(100))) {
            return false;
        }
    }

    if (check_cancelled()) return false;

    // 5. Starting
    if (progress_callback) progress_callback(RestartStage::Starting, "Launching Gateway process...");
    if (!start_gateway(config_path, error_out)) {
        if (progress_callback) progress_callback(RestartStage::Failed, error_out && !error_out->empty() ? *error_out : "Failed to launch Gateway.");
        return false;
    }

    // 6. WaitingHealth
    if (progress_callback) progress_callback(RestartStage::WaitingHealth, "Polling health endpoint...");
    uint32_t max_wait_sec = new_config.embedded_engine.enabled ? 60 : 10;
    auto health_start = std::chrono::steady_clock::now();

    while (true) {
        if (check_cancelled()) return false;

        auto health_opt = health_client_.check_health(new_config.listen.port, 200);
        if (health_opt) {
            if (!new_config.embedded_engine.enabled ||
                health_opt->embedded == "ready" ||
                health_opt->embedded == "error" ||
                health_opt->embedded == "disabled") {
                std::string completion_msg = (health_opt->embedded == "error")
                    ? "Gateway restarted (Healthy; Embedded Engine Error)."
                    : "Gateway restarted successfully.";
                if (progress_callback) progress_callback(RestartStage::Completed, completion_msg);
                return true;
            }
        } else if (!is_running()) {
            std::string exit_desc = describe_process_exit();
            cleanup_handle();
            if (error_out) *error_out = exit_desc;
            if (progress_callback) progress_callback(RestartStage::Failed, exit_desc);
            return false;
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - health_start).count() > max_wait_sec) {
            if (error_out) *error_out = "Health polling timed out after " + std::to_string(max_wait_sec) + " seconds.";
            if (progress_callback) progress_callback(RestartStage::Failed, *error_out);
            return false;
        }
        if (!sleep_cancellable(std::chrono::milliseconds(200))) {
            return false;
        }
    }
}

} // namespace maintenance
