#include "maintenance/config_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>

#include <random>
#include <fstream>
#include <sstream>

namespace maintenance {

namespace {

std::wstring to_wide_str(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring out(req, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, out.data(), req);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::filesystem::path get_exe_dir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

} // namespace

ConfigStore::ConfigStore(std::optional<std::filesystem::path> path) {
    current_path_ = path ? std::filesystem::absolute(*path) : determine_config_path();
}

bool ConfigStore::is_directory_writable_acl(const std::filesystem::path& dir) {
    std::wstring wdir = dir.wstring();
    PSECURITY_DESCRIPTOR pSD = nullptr;
    PSID pOwner = nullptr;
    PSID pGroup = nullptr;
    PACL pDacl = nullptr;

    DWORD res = GetNamedSecurityInfoW(
        wdir.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION,
        &pOwner, &pGroup, &pDacl, nullptr, &pSD
    );

    if (res != ERROR_SUCCESS || !pSD) {
        return false;
    }

    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &hToken)) {
        LocalFree(pSD);
        return false;
    }

    HANDLE hImpersonationToken = nullptr;
    if (!DuplicateToken(hToken, SecurityImpersonation, &hImpersonationToken)) {
        CloseHandle(hToken);
        LocalFree(pSD);
        return false;
    }

    GENERIC_MAPPING mapping = {};
    mapping.GenericRead = FILE_GENERIC_READ;
    mapping.GenericWrite = FILE_GENERIC_WRITE;
    mapping.GenericExecute = FILE_GENERIC_EXECUTE;
    mapping.GenericAll = FILE_ALL_ACCESS;

    PRIVILEGE_SET privileges = {};
    DWORD priv_len = sizeof(privileges);
    DWORD granted_access = 0;
    BOOL access_status = FALSE;

    MapGenericMask(&mapping.GenericWrite, &mapping);
    AccessCheck(
        pSD,
        hImpersonationToken,
        FILE_ADD_FILE | FILE_WRITE_DATA,
        &mapping,
        &privileges,
        &priv_len,
        &granted_access,
        &access_status
    );

    CloseHandle(hImpersonationToken);
    CloseHandle(hToken);
    LocalFree(pSD);

    return (access_status == TRUE);
}

std::filesystem::path ConfigStore::determine_config_path() const {
    std::string expanded_appdata = gateway::expand_environment_variables("%LOCALAPPDATA%\\LLM-Gateway\\llm-gateway.json");
    std::filesystem::path default_path = gateway::path_from_utf8(expanded_appdata);

    std::error_code ec;
    if (std::filesystem::exists(default_path, ec)) {
        return default_path;
    }

    // Check adjacent file if directory is writable
    std::filesystem::path exe_dir = get_exe_dir();
    std::filesystem::path adjacent = exe_dir / "llm-gateway.json";
    if (std::filesystem::exists(adjacent, ec) && is_directory_writable_acl(exe_dir)) {
        return adjacent;
    }

    // Default target for new config
    return default_path;
}

gateway::GatewayConfig ConfigStore::load() {
    std::error_code ec;
    if (!std::filesystem::exists(current_path_, ec)) {
        // Return default configuration
        return gateway::GatewayConfig{};
    }
    return gateway::load_config_from_file(current_path_, false);
}

bool ConfigStore::exists_on_disk() const noexcept {
    std::error_code ec;
    return std::filesystem::is_regular_file(current_path_, ec);
}

bool ConfigStore::configs_equal(const gateway::GatewayConfig& a, const gateway::GatewayConfig& b) {
    try {
        return gateway::serialize_config(a) == gateway::serialize_config(b);
    } catch (...) {
        return false;
    }
}

bool ConfigStore::validate_config(const gateway::GatewayConfig& config, std::string* error_out, bool validate_model_files) {
    try {
        nlohmann::json j = gateway::serialize_config(config);
        (void)gateway::parse_and_validate_config(j, validate_model_files);
        return true;
    } catch (const std::exception& e) {
        if (error_out) *error_out = e.what();
        return false;
    }
}

bool ConfigStore::upsert_provider(
    gateway::GatewayConfig& config,
    const gateway::ProviderConfig& provider,
    std::string* error_out) {

    if (provider.id.empty()) {
        if (error_out) *error_out = "Provider ID cannot be empty.";
        return false;
    }

    gateway::GatewayConfig candidate = config;
    bool updated = false;
    for (auto& existing : candidate.providers) {
        if (existing.id == provider.id) {
            existing = provider;
            updated = true;
            break;
        }
    }
    if (!updated) {
        candidate.providers.push_back(provider);
    }

    if (!validate_config(candidate, error_out, false)) {
        return false;
    }

    config = std::move(candidate);
    return true;
}

bool ConfigStore::remove_provider(
    gateway::GatewayConfig& config,
    const std::string& provider_id,
    std::string* error_out) {

    for (const auto& m : config.models) {
        if (m.provider == provider_id) {
            if (error_out) {
                *error_out = "Cannot remove provider '" + provider_id +
                             "' because model alias '" + m.alias + "' references it.";
            }
            return false;
        }
    }

    auto it = std::find_if(config.providers.begin(), config.providers.end(),
                           [&](const gateway::ProviderConfig& p) { return p.id == provider_id; });
    if (it == config.providers.end()) {
        if (error_out) *error_out = "Provider '" + provider_id + "' not found.";
        return false;
    }

    config.providers.erase(it);
    return true;
}

bool ConfigStore::upsert_model_alias(
    gateway::GatewayConfig& config,
    const gateway::ModelAlias& alias,
    std::string* error_out) {

    if (alias.alias.empty()) {
        if (error_out) *error_out = "Model alias name cannot be empty.";
        return false;
    }

    gateway::GatewayConfig candidate = config;
    bool updated = false;
    for (auto& existing : candidate.models) {
        if (existing.alias == alias.alias) {
            existing = alias;
            updated = true;
            break;
        }
    }
    if (!updated) {
        candidate.models.push_back(alias);
    }

    if (!validate_config(candidate, error_out, false)) {
        return false;
    }

    config = std::move(candidate);
    return true;
}

bool ConfigStore::remove_model_alias(
    gateway::GatewayConfig& config,
    const std::string& alias_name,
    std::string* error_out) {

    auto it = std::find_if(config.models.begin(), config.models.end(),
                           [&](const gateway::ModelAlias& m) { return m.alias == alias_name; });
    if (it == config.models.end()) {
        if (error_out) *error_out = "Model alias '" + alias_name + "' not found.";
        return false;
    }

    config.models.erase(it);
    return true;
}

gateway::ProviderConfig ConfigStore::make_openrouter_preset() {
    gateway::ProviderConfig p;
    p.id = "openrouter";
    p.base_url = "https://openrouter.ai/api/v1";
    p.authentication = gateway::Authentication::Bearer;
    p.key_vendor = "openrouter";
    p.key_slot = "default";
    p.model_discovery = true;
    p.allowlist = {"*"};
    return p;
}

gateway::ProviderConfig ConfigStore::make_lm_studio_preset() {
    gateway::ProviderConfig p;
    p.id = "lm-studio";
    p.base_url = "http://127.0.0.1:1234/v1";
    p.authentication = gateway::Authentication::None;
    p.key_vendor.clear();
    p.key_slot.clear();
    p.model_discovery = true;
    p.allowlist = {"*"};
    return p;
}

std::vector<std::string> ConfigStore::split_csv_tokens(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        size_t start = item.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = item.find_last_not_of(" \t\r\n");
        out.push_back(item.substr(start, end - start + 1));
    }
    return out;
}

std::string ConfigStore::join_csv_tokens(const std::vector<std::string>& items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ", ";
        out += items[i];
    }
    return out;
}

bool ConfigStore::save_atomic(const gateway::GatewayConfig& config, std::string* error_out) {
    nlohmann::json j;
    std::string json_str;
    try {
        j = gateway::serialize_config(config);
        (void)gateway::parse_and_validate_config(j, false);
        json_str = j.dump(2);
    } catch (const std::exception& e) {
        if (error_out) *error_out = std::string("Configuration validation failed: ") + e.what();
        return false;
    }

    std::filesystem::path parent = current_path_.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
    }

    // Generate random temporary filename using wide strings (Unicode-safe)
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::wstring tmp_filename = current_path_.filename().wstring() + L"." + std::to_wstring(dist(gen)) + L".tmp";
    std::filesystem::path tmp_path = parent / tmp_filename;

    std::wstring wtmp = tmp_path.wstring();
    std::wstring wtarget = current_path_.wstring();

    HANDLE hFile = CreateFileW(
        wtmp.c_str(),
        GENERIC_WRITE,
        0, // Exclusive access
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        if (error_out) *error_out = "Failed to create temporary file: " + gateway::path_to_utf8(tmp_path) + " (Error=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    struct TempFileGuard {
        HANDLE handle;
        const std::wstring& path;
        bool keep_file = false;
        void close_handle() {
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
        }
        ~TempFileGuard() {
            close_handle();
            if (!keep_file) {
                DeleteFileW(path.c_str());
            }
        }
    } guard{hFile, wtmp, false};

    DWORD written = 0;
    BOOL write_ok = WriteFile(
        guard.handle,
        json_str.data(),
        static_cast<DWORD>(json_str.size()),
        &written,
        nullptr
    );

    if (!write_ok || written != json_str.size()) {
        if (error_out) *error_out = "Failed to write data to temporary file (Error=" + std::to_string(GetLastError()) + ").";
        return false;
    }

    if (!FlushFileBuffers(guard.handle)) {
        if (error_out) *error_out = "FlushFileBuffers failed on temporary config file (Error=" + std::to_string(GetLastError()) + ").";
        return false;
    }
    guard.close_handle();

    // Atomic replace
    BOOL move_ok = MoveFileExW(
        wtmp.c_str(),
        wtarget.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
    );

    if (!move_ok) {
        if (error_out) *error_out = "MoveFileExW failed to atomically replace config file (Error=" + std::to_string(GetLastError()) + ")";
        return false;
    }

    guard.keep_file = true;
    return true;
}

} // namespace maintenance
