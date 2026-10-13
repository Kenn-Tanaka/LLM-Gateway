#pragma once

#include "gateway/config.hpp"
#include "gateway/error.hpp"

#include <string>
#include <optional>
#include <variant>
#include <vector>

namespace gateway {

enum class EndpointType {
    ChatCompletions,
    Embeddings,
    Rerank
};

struct ResolvedRoute {
    std::string canonical_model_id; // e.g. "openrouter/anthropic/claude-3" or "embedded/embedding_model"
    std::string provider_id;        // e.g. "openrouter" or "embedded"
    std::string upstream_model;     // e.g. "anthropic/claude-3"
    std::optional<std::string> input_prefix; // only applicable for /v1/embeddings
    bool is_embedded = false;
};

class ModelRouter {
public:
    explicit ModelRouter(const GatewayConfig& config);

    // Resolves model request and checks allowlist.
    // Returns ResolvedRoute or GatewayError (404/403).
    [[nodiscard]] std::variant<ResolvedRoute, GatewayError> resolve(
        const std::string& requested_model,
        EndpointType endpoint) const;

    // Direct check for allowlist pattern match
    [[nodiscard]] static bool matches_allowlist(
        const std::string& canonical_id,
        const std::vector<std::string>& allowlist);

private:
    const GatewayConfig& config_;
    std::string embedding_model_filename_;
    std::string reranker_model_filename_;

    [[nodiscard]] static std::string extract_filename(const std::string& path);
};

} // namespace gateway
