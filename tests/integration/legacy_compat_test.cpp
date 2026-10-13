#include "gateway/config.hpp"
#include "gateway/embedded_engine.hpp"
#include "gateway/external_proxy.hpp"
#include "gateway/model_catalog.hpp"
#include "gateway/request_guard.hpp"
#include "gateway/logger.hpp"

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

// Synthetic fixture only: the user's private configuration is never distributed.
nlohmann::json legacy_fixture() {
    return nlohmann::json::parse(R"({
      "listen":{"address":"127.0.0.1","port":8765},
      "security":{"client_auth":false,"credential_name":"llm_gateway|default"},
      "credentials":{"service_name":"LegacyCompatibilityTest","environment_fallback":false},
      "proxy":{"max_request_body_mb":64,"connect_timeout_sec":10,"read_timeout_sec":600,"write_timeout_sec":600},
      "model_cache":{"enabled":true,"ttl_sec":60},
      "cors":{"enabled":true,"allowed_origins":["http://localhost:5173"],"allowed_methods":["GET","POST","DELETE","PUT","PATCH","OPTIONS"],"allowed_headers":["Authorization","Content-Type"],"max_age_sec":120},
      "providers":{
        "local":{"base_url":"http://127.0.0.1:1234/v1","authentication":"none","model_discovery":true,"allowlist":[{"type":"glob","pattern":"local/*"}]},
        "cloud":{"base_url":"https://example.invalid/api","authentication":"bearer","key_vendor":"test","key_slot":"default","model_discovery":true,"allowlist":[{"type":"glob","pattern":"cloud/*"}]}
      },
      "logging":{"level":"info","file":"llm-gateway.log"}
    })");
}

void parsing() {
    const auto original = legacy_fixture();
    const auto config = gateway::parse_and_validate_config(original);
    require(config.loaded_from_legacy, "legacy provenance");
    require(config.listen.host == "127.0.0.1" && config.listen.port == 8765, "listen mapping");
    require(!config.embedded_engine.enabled && !config.security.client_auth, "legacy external-only defaults");
    require(config.limits.max_request_bytes == 67108864, "MiB conversion");
    require(config.timeouts.upstream_connect_ms == 10000 && config.timeouts.upstream_read_ms == 600000 && config.timeouts.upstream_write_ms == 600000, "timeout conversion");
    require(config.model_cache.enabled && config.model_cache.ttl_sec == 60, "cache mapping");
    require(config.logging.level == "INFO" && config.logging.file == "llm-gateway.log", "logging mapping");
    require(config.find_provider("local")->base_url == "http://127.0.0.1:1234/v1", "avoid duplicate v1");
    require(config.find_provider("cloud")->base_url == "https://example.invalid/api/v1", "API root mapping");
    require(config.find_provider("cloud")->allowlist == std::vector<std::string>{"cloud/*"}, "allowlist mapping");
    const auto serialized = gateway::serialize_config(config);
    require(!gateway::parse_and_validate_config(serialized).loaded_from_legacy, "versioned provenance");
    require(gateway::serialize_config(gateway::parse_and_validate_config(serialized)) == serialized, "save/reload preserves compatibility values");
    require(original == legacy_fixture(), "parser must not mutate input");
    auto rejected = [](const nlohmann::json& j) {
        try { gateway::parse_and_validate_config(j); } catch (const std::exception&) { return true; }
        return false;
    };
    auto bad = original; bad["proxy"]["read_timeout_sec"] = -1;
    require(rejected(bad), "negative legacy timeout accepted");
    bad = original; bad["proxy"]["max_request_body_mb"] = 1.5;
    require(rejected(bad), "fractional limit accepted");
    bad = original; bad["proxy"]["max_request_body_mb"] = UINT64_MAX;
    require(rejected(bad), "overflow limit accepted");
    bad = original; bad["providers"]["local"]["allowlist"][0]["type"] = "regex";
    require(rejected(bad), "unsupported rule accepted");
    bad = original; bad["unexpected"] = true;
    require(rejected(bad), "unknown legacy field accepted");
    bad = original; bad["schema_version"] = "2.0";
    require(rejected(bad), "versioned legacy mixture accepted");
    bad = serialized; bad["schema_version"] = "3.0";
    require(rejected(bad), "unsupported schema accepted");
    bad = serialized; bad["embedded_engine"]["embedding_model"] = {{"path", "missing-test-model.gguf"}, {"n_threads", 1}};
    require(!gateway::parse_and_validate_config(bad).embedded_engine.enabled, "disabled model checked on disk");

    gateway::RequestGuard guard(config);
    httplib::Request request;
    request.method = "OPTIONS";
    request.headers.emplace("Origin", "http://localhost:5173");
    request.headers.emplace("Access-Control-Request-Method", "PATCH");
    request.headers.emplace("Access-Control-Request-Headers", "authorization, content-type");
    httplib::Response response;
    require(guard.handle_cors_preflight(request, response) && response.status == 204, "legacy CORS methods");
    require(response.get_header_value("Access-Control-Max-Age") == "120", "legacy CORS max-age");
    auto disabled = config; disabled.security.cors_enabled = false;
    gateway::RequestGuard disabled_guard(disabled);
    httplib::Response denied;
    require(disabled_guard.handle_cors_preflight(request, denied) && denied.status == 403, "disabled CORS");
}

void mock_provider() {
    httplib::Server mock;
    std::atomic<int> calls{0};
    std::atomic<int> chat_calls{0};
    mock.Get("/api/v1/models", [&](const auto&, auto& response) {
        ++calls;
        response.set_content(R"({"data":[]})", "application/json");
    });
    mock.Post("/api/v1/chat/completions", [&](const auto& request, auto& response) {
        ++chat_calls;
        auto parsed = nlohmann::json::parse(request.body, nullptr, false);
        if (parsed.is_object() && parsed.value("stream", false)) {
            response.set_chunked_content_provider("text/event-stream", [](size_t, httplib::DataSink& sink) {
                const char* msg = "data: {\"ok\":true}\n\ndata: [DONE]\n\n";
                sink.write(msg, std::strlen(msg));
                sink.done();
                return true;
            });
            return;
        }
        response.set_content(request.body, "application/json");
    });
    const auto port = mock.bind_to_any_port("127.0.0.1");
    require(port > 0, "mock bind failed");
    std::thread worker([&] { mock.listen_after_bind(); });
    try {
        auto j = legacy_fixture();
        j["providers"].erase("local");
        j["providers"]["cloud"]["authentication"] = "none";
        j["providers"]["cloud"]["base_url"] = "http://127.0.0.1:" + std::to_string(port) + "/api";
        auto config = gateway::parse_and_validate_config(j);
        gateway::CredentialStore store("LegacyCompatibilityTest", false);
        gateway::EmbeddedEngine engine(config, true);
        gateway::ExternalProxy proxy(config, store);
        gateway::ModelCatalog catalog(config, store);
        require(std::holds_alternative<nlohmann::json>(catalog.get_models_list(engine)), "discovery request failed");
        require(std::holds_alternative<nlohmann::json>(catalog.get_models_list(engine)) && calls == 1, "empty successful list must be cached");
        config.model_cache.enabled = false;
        require(std::holds_alternative<nlohmann::json>(catalog.get_models_list(engine)), "uncached discovery failed");
        require(calls == 2, "cache enabled flag ignored");
        config.model_cache.enabled = true; config.model_cache.ttl_sec = 0;
        require(std::holds_alternative<nlohmann::json>(catalog.get_models_list(engine)), "expired discovery failed");
        require(calls == 3, "cache TTL ignored");
        httplib::Request request; request.method = "POST";
        httplib::Response response;
        const std::string nested_body = R"({"model":"test","metadata":{"stream":true},"stream":false})";
        proxy.forward_request(*config.find_provider("cloud"), "/chat/completions", request, response, nested_body);
        require(response.status == 200 && response.body == nested_body && chat_calls == 1, "legacy API-root forwarding");
    } catch (...) { mock.stop(); worker.join(); throw; }
    mock.stop(); worker.join();
}
}

int main(int argc, char** argv) {
    try {
        parsing();
        mock_provider();
        if (argc == 2) {
            const auto config = gateway::load_config_from_file(gateway::path_from_utf8(argv[1]));
            require(!config.providers.empty(), "private config has no providers");
            std::cout << "Private config loaded read-only; providers=" << config.providers.size() << '\n';
        }
        std::cout << "Legacy compatibility checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Legacy compatibility check failed: " << error.what() << '\n';
        return 1;
    }
}
