#pragma once

#include "gateway/config.hpp"
#include <filesystem>
#include <string>
#include <optional>
#include <vector>

namespace maintenance {

class ConfigStore {
public:
    explicit ConfigStore(std::optional<std::filesystem::path> path = std::nullopt);

    // Resolves appropriate config path (prefers LocalAppData, falls back to adjacent if writable)
    [[nodiscard]] std::filesystem::path determine_config_path() const;

    // Loads config from current path
    [[nodiscard]] gateway::GatewayConfig load();

    // Saves config atomically using MoveFileExW
    bool save_atomic(const gateway::GatewayConfig& config, std::string* error_out = nullptr);

    [[nodiscard]] const std::filesystem::path& current_path() const noexcept {
        return current_path_;
    }

    void set_path(const std::filesystem::path& p) {
        current_path_ = std::filesystem::absolute(p);
    }

    [[nodiscard]] bool exists_on_disk() const noexcept;

    // Compares two configurations by their canonical serialized JSON representation
    [[nodiscard]] static bool configs_equal(const gateway::GatewayConfig& a, const gateway::GatewayConfig& b);

    // Validates a GatewayConfig via common parser without touching disk
    [[nodiscard]] static bool validate_config(
        const gateway::GatewayConfig& config,
        std::string* error_out = nullptr,
        bool validate_model_files = false);

    // Validated CRUD helpers for GUI and programmatic setup
    static bool upsert_provider(
        gateway::GatewayConfig& config,
        const gateway::ProviderConfig& provider,
        std::string* error_out = nullptr);

    static bool remove_provider(
        gateway::GatewayConfig& config,
        const std::string& provider_id,
        std::string* error_out = nullptr);

    static bool upsert_model_alias(
        gateway::GatewayConfig& config,
        const gateway::ModelAlias& alias,
        std::string* error_out = nullptr);

    static bool remove_model_alias(
        gateway::GatewayConfig& config,
        const std::string& alias_name,
        std::string* error_out = nullptr);

    [[nodiscard]] static gateway::ProviderConfig make_openrouter_preset();
    [[nodiscard]] static gateway::ProviderConfig make_lm_studio_preset();

    [[nodiscard]] static std::vector<std::string> split_csv_tokens(const std::string& csv);
    [[nodiscard]] static std::string join_csv_tokens(const std::vector<std::string>& items);

private:
    std::filesystem::path current_path_;

    static bool is_directory_writable_acl(const std::filesystem::path& dir);
};

} // namespace maintenance
