#pragma once

#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/error.hpp"

#include <string>
#include <vector>
#include <variant>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <nlohmann/json.hpp>

namespace gateway {

class EmbeddedEngine; // forward declaration

struct ProviderModelCache {
    std::chrono::steady_clock::time_point fetched_at;
    std::vector<std::string> discovered_models;
};

class ModelCatalog {
public:
    ModelCatalog(const GatewayConfig& config, const CredentialStore& store);

    // Generates /v1/models response JSON
    [[nodiscard]] std::variant<nlohmann::json, GatewayError> get_models_list(
        const EmbeddedEngine& embedded_engine) const;

private:
    const GatewayConfig& config_;
    const CredentialStore& store_;

    mutable std::mutex cache_mutex_;
    mutable std::unordered_map<std::string, ProviderModelCache> cache_;

    [[nodiscard]] std::optional<std::vector<std::string>> fetch_provider_models(
        const ProviderConfig& provider) const;
};

} // namespace gateway
