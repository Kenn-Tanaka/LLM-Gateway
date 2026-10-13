#include "gateway/model_router.hpp"

#include <filesystem>

namespace gateway {

std::string ModelRouter::extract_filename(const std::string& path) {
    if (path.empty()) return "";
    return std::filesystem::path(path).filename().string();
}

ModelRouter::ModelRouter(const GatewayConfig& config)
    : config_(config) {
    if (config_.embedded_engine.embedding_model) {
        embedding_model_filename_ = extract_filename(config_.embedded_engine.embedding_model->path);
    }
    if (config_.embedded_engine.reranker_model) {
        reranker_model_filename_ = extract_filename(config_.embedded_engine.reranker_model->path);
    }
}

bool ModelRouter::matches_allowlist(
    const std::string& canonical_id,
    const std::vector<std::string>& allowlist) {

    if (allowlist.empty()) return false;

    for (const auto& pattern : allowlist) {
        if (pattern == "*") return true;

        if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == "/*") {
            std::string prefix = pattern.substr(0, pattern.size() - 1); // includes trailing '/'
            if (canonical_id.rfind(prefix, 0) == 0 && canonical_id.size() > prefix.size()) {
                return true;
            }
        } else if (canonical_id == pattern) {
            return true;
        }
    }
    return false;
}

std::variant<ResolvedRoute, GatewayError> ModelRouter::resolve(
    const std::string& requested_model,
    EndpointType endpoint) const {

    std::string provider_id;
    std::string upstream_model;
    std::optional<std::string> input_prefix;
    bool is_embedded = false;

    // 1. Check if requested_model is an Alias
    const ModelAlias* alias = config_.find_alias(requested_model);
    if (alias != nullptr) {
        provider_id = alias->provider;
        upstream_model = alias->model;
        if (endpoint == EndpointType::Embeddings) {
            input_prefix = alias->input_prefix;
        }
    } else {
        // 2. Not an alias, split by first '/'
        size_t slash_pos = requested_model.find('/');
        if (slash_pos == std::string::npos || slash_pos == 0 || slash_pos == requested_model.size() - 1) {
            return errors::provider_not_found();
        }

        provider_id = requested_model.substr(0, slash_pos);
        upstream_model = requested_model.substr(slash_pos + 1);
    }

    // 3. Check Provider existence
    const ProviderConfig* provider = config_.find_provider(provider_id);
    if (provider == nullptr) {
        return errors::provider_not_found();
    }

    // 4. Handle embedded provider
    if (provider_id == "embedded") {
        is_embedded = true;

        if (endpoint == EndpointType::ChatCompletions) {
            return errors::endpoint_not_found();
        }

        if (endpoint == EndpointType::Embeddings) {
            // Direct ID must be embedded/embedding_model or embedded/<embedding-file-name>
            if (alias == nullptr) {
                if (upstream_model != "embedding_model" && upstream_model != embedding_model_filename_) {
                    return errors::endpoint_not_found();
                }
            }
        } else if (endpoint == EndpointType::Rerank) {
            // Direct ID must be embedded/reranker_model or embedded/<reranker-file-name>
            if (alias == nullptr) {
                if (upstream_model != "reranker_model" && upstream_model != reranker_model_filename_) {
                    return errors::endpoint_not_found();
                }
            }
        }
    } else {
        // Non-embedded route for /v1/rerank is invalid
        if (endpoint == EndpointType::Rerank) {
            return errors::unsupported_route();
        }
    }

    // 5. Construct canonical model ID
    std::string canonical_id = provider_id + "/" + upstream_model;

    // 6. Check Allowlist
    if (!matches_allowlist(canonical_id, provider->allowlist)) {
        return errors::gateway_model_forbidden();
    }

    ResolvedRoute route;
    route.canonical_model_id = std::move(canonical_id);
    route.provider_id = std::move(provider_id);
    route.upstream_model = std::move(upstream_model);
    route.input_prefix = std::move(input_prefix);
    route.is_embedded = is_embedded;

    return route;
}

} // namespace gateway
