#include "gateway/model_router.hpp"
#include <cassert>
#include <iostream>

void test_router() {
    gateway::GatewayConfig config;
    config.listen.port = 8765;

    gateway::ProviderConfig prov;
    prov.id = "openrouter";
    prov.base_url = "https://openrouter.ai/api/v1";
    prov.allowlist = {"openrouter/anthropic/*", "openrouter/meta/llama-3"};
    config.providers.push_back(prov);

    gateway::ModelAlias ma;
    ma.alias = "claude";
    ma.provider = "openrouter";
    ma.model = "anthropic/claude-3.5";
    config.models.push_back(ma);

    gateway::ModelRouter router(config);

    // 1. Alias match and allowlist pass
    auto res1 = router.resolve("claude", gateway::EndpointType::ChatCompletions);
    assert(std::holds_alternative<gateway::ResolvedRoute>(res1));
    const auto& r1 = std::get<gateway::ResolvedRoute>(res1);
    assert(r1.canonical_model_id == "openrouter/anthropic/claude-3.5");
    assert(r1.upstream_model == "anthropic/claude-3.5");

    // 2. Direct provider/model with wildcard prefix pass
    auto res2 = router.resolve("openrouter/anthropic/claude-instant", gateway::EndpointType::ChatCompletions);
    assert(std::holds_alternative<gateway::ResolvedRoute>(res2));

    // 3. Direct provider/model with exact match pass
    auto res3 = router.resolve("openrouter/meta/llama-3", gateway::EndpointType::ChatCompletions);
    assert(std::holds_alternative<gateway::ResolvedRoute>(res3));

    // 4. Denied model by allowlist (default deny)
    auto res4 = router.resolve("openrouter/openai/gpt-4", gateway::EndpointType::ChatCompletions);
    assert(std::holds_alternative<gateway::GatewayError>(res4));
    assert(std::get<gateway::GatewayError>(res4).code == "gateway_model_forbidden");

    // 5. Undefined provider
    auto res5 = router.resolve("unknown/model-1", gateway::EndpointType::ChatCompletions);
    assert(std::holds_alternative<gateway::GatewayError>(res5));
    assert(std::get<gateway::GatewayError>(res5).code == "provider_not_found");

    std::cout << "[PASS] ModelRouter unit tests.\n";
}

int main() {
    test_router();
    return 0;
}
