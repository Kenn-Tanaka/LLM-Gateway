#pragma once

#include "gateway/config.hpp"
#include "maintenance/health_client.hpp"
#include "gateway/credential_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <optional>
#include <functional>
#include <atomic>
#include <mutex>

namespace maintenance {

enum class RestartStage {
    Validating,
    Saving,
    Stopping,
    WaitingPortRelease,
    Starting,
    WaitingHealth,
    Completed,
    Failed
};

class ProcessController {
public:
    ProcessController(std::filesystem::path gateway_exe_path, const gateway::CredentialStore& cred_store);
    ~ProcessController();

    // Starts gateway process completely hidden
    bool start_gateway(const std::filesystem::path& config_path, std::string* error_out = nullptr);

    // Stops gateway process (graceful via HTTP /admin/shutdown, fallback to TerminateProcess)
    bool stop_gateway(uint16_t port, std::string* error_out = nullptr, const std::string& admin_credential_name = "");

    // Re-attaches to existing running gateway process via Toolhelp snapshot & path verification
    bool reattach_gateway(uint16_t port);

    // Pipeline for save & restart
    bool restart_pipeline(
        const gateway::GatewayConfig& new_config,
        const std::filesystem::path& config_path,
        std::function<void(RestartStage, const std::string&)> progress_callback,
        std::string* error_out = nullptr);

    void set_active_admin_credential_name(std::string name);
    void set_active_port(uint16_t port) noexcept;
    [[nodiscard]] uint16_t active_port() const noexcept;
    void request_cancel() noexcept;
    void reset_cancel() noexcept;
    [[nodiscard]] bool is_cancel_requested() const noexcept;

    [[nodiscard]] bool is_running() const noexcept;
    [[nodiscard]] DWORD process_id() const noexcept;
    [[nodiscard]] const std::filesystem::path& gateway_exe_path() const noexcept { return gateway_exe_path_; }

private:
    std::filesystem::path gateway_exe_path_;
    const gateway::CredentialStore& cred_store_;
    HealthClient health_client_;

    mutable std::mutex mutex_;
    std::atomic<bool> cancel_requested_{false};
    HANDLE process_handle_ = nullptr;
    HANDLE stderr_read_pipe_ = nullptr;
    DWORD pid_ = 0;
    uint16_t active_port_ = 0;
    std::string active_admin_credential_name_ = "llm_gateway|admin";
    std::filesystem::path last_config_path_;

    void cleanup_handle();
    void cleanup_handle_locked();
    [[nodiscard]] std::string drain_stderr_pipe_locked();
    [[nodiscard]] std::string describe_process_exit();
    [[nodiscard]] bool is_matching_gateway_process(DWORD target_pid) const;
};

} // namespace maintenance
