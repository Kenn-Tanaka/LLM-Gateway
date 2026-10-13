#pragma once

#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace gateway {

enum class Authentication {
    None,
    Bearer
};

struct ListenConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 8765;
};

struct LimitsConfig {
    size_t max_request_bytes = 10485760; // 10MB default
};

struct TimeoutsConfig {
    uint32_t upstream_connect_ms = 5000;
    uint32_t upstream_read_ms = 60000;
    uint32_t upstream_write_ms = 60000;
};

struct ModelCacheConfig {
    bool enabled = true;
    uint32_t ttl_sec = 3600;
};

struct LoggingConfig {
    std::string level = "INFO";
    std::string dir = "%LOCALAPPDATA%\\LLM-Gateway\\logs";
    std::optional<std::string> file; // Legacy fixed file, relative to process working directory.
};

struct SecurityConfig {
    bool client_auth = true;
    std::string credential_name = "llm_gateway|default";
    std::string admin_credential_name = "llm_gateway|admin";
    std::vector<std::string> allowed_origins;
    bool cors_enabled = true;
    std::vector<std::string> allowed_methods = {"GET", "POST", "OPTIONS"};
    std::vector<std::string> allowed_headers = {"Authorization", "Content-Type"};
    uint32_t cors_max_age_sec = 600;
};

struct CredentialsConfig {
    std::string service_name = "CloudLLM";
    bool environment_fallback = false;
};

struct ModelFileConfig {
    std::string path;
    int n_threads = 0; // 0 indicates default calculation: max(1, hardware_concurrency/2)
    int n_gpu_layers = 0; // must be 0
};

struct EmbeddedEngineConfig {
    bool enabled = true;
    std::optional<ModelFileConfig> embedding_model;
    std::optional<ModelFileConfig> reranker_model;
};

struct ModelAlias {
    std::string alias;
    std::string provider;
    std::string model;
    std::optional<std::string> input_prefix;
};

struct ProviderConfig {
    std::string id;
    std::optional<std::string> base_url;
    Authentication authentication = Authentication::None;
    std::string key_vendor;
    std::string key_slot;
    bool model_discovery = false;
    std::vector<std::string> allowlist;
};

struct GatewayConfig {
    bool loaded_from_legacy = false; // In-memory provenance, never serialized.
    std::string schema_version = "2.0";
    ListenConfig listen;
    LimitsConfig limits;
    TimeoutsConfig timeouts;
    ModelCacheConfig model_cache;
    LoggingConfig logging;
    SecurityConfig security;
    CredentialsConfig credentials;
    EmbeddedEngineConfig embedded_engine;
    std::vector<ModelAlias> models;
    std::vector<ProviderConfig> providers;

    [[nodiscard]] const ProviderConfig* find_provider(const std::string& provider_id) const;
    [[nodiscard]] const ModelAlias* find_alias(const std::string& alias_name) const;
};

// Path resolution and UTF-8 conversion helpers
std::filesystem::path path_from_utf8(std::string_view utf8_path);
std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path resolve_config_path(const std::optional<std::string>& cli_config_path, bool is_development_mode = false);
std::string expand_environment_variables(const std::string& input);

// JSON serialization / deserialization with strict validation
GatewayConfig parse_and_validate_config(const nlohmann::json& j, bool validate_model_files = false);
GatewayConfig load_config_from_file(const std::filesystem::path& path, bool validate_model_files = false);
nlohmann::json serialize_config(const GatewayConfig& config);

} // namespace gateway
