#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/embedded_engine.hpp"
#include "gateway/shutdown_coordinator.hpp"
#include "gateway/http_server.hpp"
#include "gateway/logger.hpp"
#include "maintenance/health_client.hpp"
#include "maintenance/process_controller.hpp"
#include "maintenance/config_store.hpp"
#include "maintenance/credential_editor.hpp"
#include "maintenance/log_tail_reader.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

size_t write_string_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t header_capture_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::vector<std::string>*>(userdata);
    headers->emplace_back(buffer, size * nitems);
    return size * nitems;
}

struct SimpleHttpResponse {
    long status = 0;
    std::string body;
    std::vector<std::string> headers;
};

SimpleHttpResponse perform_http(
    const std::string& method,
    const std::string& url,
    const std::string& body = "",
    const std::vector<std::string>& extra_headers = {},
    long timeout_ms = 5000) {

    SimpleHttpResponse res;
    CURL* curl = curl_easy_init();
    assert(curl);

    curl_slist* hdrs = nullptr;
    for (const auto& h : extra_headers) {
        hdrs = curl_slist_append(hdrs, h.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_capture_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    if (!body.empty() || method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }

    CURLcode code = curl_easy_perform(curl);
    assert(code == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

bool has_header_containing(const std::vector<std::string>& headers, const std::string& needle) {
    for (const auto& h : headers) {
        if (h.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

void test_health_endpoint() {
    gateway::GatewayConfig config;
    config.listen.host = "127.0.0.1";
    config.listen.port = 18765; // dedicated test port
    config.embedded_engine.enabled = false;

    gateway::CredentialStore store("LLMGatewayPhase1HealthTest_Synthetic", false);
    gateway::EmbeddedEngine embedded_engine(config, true);
    gateway::ShutdownCoordinator shutdown_coordinator;

    gateway::HttpServer server(config, store, embedded_engine, shutdown_coordinator);

    std::thread server_thread([&server]() {
        server.start();
    });

    server.wait_until_ready();

    maintenance::HealthClient client;
    auto health_opt = client.check_health(18765, 2000);

    assert(health_opt.has_value());
    assert(health_opt->status == "ok");
    assert(health_opt->version == "2.0.0");
    assert(health_opt->embedded == "disabled");
    assert(health_opt->pid > 0);
    assert(shutdown_coordinator.active_requests() == 0);

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    std::cout << "[PASS] Health endpoint integration test.\n";
}

void test_h01_sse_streaming_and_upstream_proxy() {
    httplib::Server mock_upstream;
    std::atomic<int> upstream_post_calls{0};
    std::atomic<int> disconnect_chunks_sent{0};
    std::atomic<bool> disconnect_stream_terminated{false};

    mock_upstream.Post("/v1/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
        ++upstream_post_calls;
        auto j = nlohmann::json::parse(req.body);
        std::string model = j.value("model", "");

        if (model == "sse-ok" || model == "mock/sse-ok") {
            res.set_chunked_content_provider(
                "text/event-stream; charset=utf-8",
                [](size_t /*offset*/, httplib::DataSink& sink) {
                    const char* c1 = "data: {\"chunk\":1}\n\n";
                    if (!sink.write(c1, std::strlen(c1))) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));

                    const char* c2 = "data: {\"chunk\":2}\n\n";
                    if (!sink.write(c2, std::strlen(c2))) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));

                    const char* c3 = "data: [DONE]\n\n";
                    if (!sink.write(c3, std::strlen(c3))) return false;
                    sink.done();
                    return true;
                }
            );
            return;
        }

        if (model == "sse-429" || model == "mock/sse-429") {
            res.status = 429;
            res.set_header("Retry-After", "7");
            res.set_content(R"({"error":{"message":"Upstream rate limit","code":429}})", "application/json");
            return;
        }

        if (model == "sse-disconnect" || model == "mock/sse-disconnect") {
            res.set_chunked_content_provider(
                "text/event-stream",
                [&](size_t /*offset*/, httplib::DataSink& sink) {
                    for (int i = 0; i < 60; ++i) {
                        if (sink.is_writable && !sink.is_writable()) {
                            disconnect_stream_terminated.store(true);
                            return false;
                        }
                        std::string chunk = "data: {\"seq\":" + std::to_string(i) + "}\n\n";
                        if (!sink.write(chunk.data(), chunk.size())) {
                            disconnect_stream_terminated.store(true);
                            return false;
                        }
                        disconnect_chunks_sent.fetch_add(1);
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    sink.done();
                    return true;
                },
                [&](bool /*success*/) {
                    disconnect_stream_terminated.store(true);
                }
            );
            return;
        }

        // Non-streaming JSON response
        res.status = 200;
        res.set_content(R"({"id":"chatcmpl-nonstream","object":"chat.completion"})", "application/json");
    });

    int mock_port = mock_upstream.bind_to_any_port("127.0.0.1");
    assert(mock_port > 0);
    std::thread mock_thread([&]() { mock_upstream.listen_after_bind(); });

    gateway::GatewayConfig config;
    config.listen.host = "127.0.0.1";
    config.listen.port = 18767;
    config.embedded_engine.enabled = false;
    config.security.client_auth = false;

    gateway::ProviderConfig prov;
    prov.id = "mock";
    prov.base_url = "http://127.0.0.1:" + std::to_string(mock_port) + "/v1";
    prov.authentication = gateway::Authentication::None;
    prov.allowlist = {"mock/*"};
    config.providers.push_back(prov);

    gateway::CredentialStore store("LLMGatewayPhase1H01Test_Synthetic", false);
    gateway::EmbeddedEngine embedded_engine(config, true);
    gateway::ShutdownCoordinator shutdown_coordinator;
    gateway::HttpServer server(config, store, embedded_engine, shutdown_coordinator);

    std::thread server_thread([&server]() { server.start(); });
    server.wait_until_ready();

    const std::string gateway_url = "http://127.0.0.1:18767/v1/chat/completions";

    // 1. True incremental SSE streaming with single upstream POST call
    {
        upstream_post_calls.store(0);
        struct StreamCapture {
            std::string full_body;
            std::vector<std::chrono::steady_clock::time_point> chunk_times;
        } cap;

        CURL* curl = curl_easy_init();
        assert(curl);
        curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
        std::string req_body = R"({"model":"mock/sse-ok","messages":[{"role":"user","content":"hi"}],"stream":true})";

        curl_easy_setopt(curl, CURLOPT_URL, gateway_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        std::vector<std::string> resp_headers;
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_capture_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);

        auto write_cb = +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            auto* c = static_cast<StreamCapture*>(userdata);
            size_t total = size * nmemb;
            c->full_body.append(ptr, total);
            c->chunk_times.push_back(std::chrono::steady_clock::now());
            return total;
        };
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cap);

        CURLcode code = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);

        assert(code == CURLE_OK);
        assert(http_code == 200);
        assert(upstream_post_calls.load() == 1); // Never called twice!
        assert(has_header_containing(resp_headers, "text/event-stream"));
        assert(cap.full_body.find("data: {\"chunk\":1}\n\n") != std::string::npos);
        assert(cap.full_body.find("data: {\"chunk\":2}\n\n") != std::string::npos);
        assert(cap.full_body.find("data: [DONE]\n\n") != std::string::npos);
        assert(cap.chunk_times.size() >= 2);
        auto span_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            cap.chunk_times.back() - cap.chunk_times.front()).count();
        assert(span_ms >= 150); // Proves incremental delivery rather than buffering until completion
        assert(shutdown_coordinator.active_requests() == 0);
    }

    // 2. Upstream 429 error on stream:true propagates exact status, headers, and JSON body
    {
        upstream_post_calls.store(0);
        auto res = perform_http(
            "POST",
            gateway_url,
            R"({"model":"mock/sse-429","messages":[{"role":"user","content":"hi"}],"stream":true})",
            {"Content-Type: application/json"}
        );
        assert(upstream_post_calls.load() == 1);
        assert(res.status == 429);
        assert(has_header_containing(res.headers, "application/json"));
        assert(has_header_containing(res.headers, "Retry-After: 7"));
        assert(res.body == R"({"error":{"message":"Upstream rate limit","code":429}})");
        assert(shutdown_coordinator.active_requests() == 0);
    }

    // 3. Client disconnect mid-stream cancels upstream transfer promptly
    {
        upstream_post_calls.store(0);
        disconnect_chunks_sent.store(0);
        disconnect_stream_terminated.store(false);

        CURL* curl = curl_easy_init();
        assert(curl);
        curl_slist* hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
        std::string req_body = R"({"model":"mock/sse-disconnect","messages":[{"role":"user","content":"hi"}],"stream":true})";

        curl_easy_setopt(curl, CURLOPT_URL, gateway_url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req_body.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

        // Abort immediately upon receiving the first chunk
        auto abort_cb = +[](char* /*ptr*/, size_t /*size*/, size_t /*nmemb*/, void* /*userdata*/) -> size_t {
            return 0;
        };
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, abort_cb);

        auto t0 = std::chrono::steady_clock::now();
        CURLcode code = curl_easy_perform(curl);
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        assert(code == CURLE_WRITE_ERROR);

        // Wait for Gateway active_requests() to return to 0 and upstream handler to terminate
        while ((shutdown_coordinator.active_requests() != 0 || !disconnect_stream_terminated.load()) &&
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count() < 1500) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        assert(upstream_post_calls.load() == 1);
        assert(shutdown_coordinator.active_requests() == 0);
        assert(disconnect_stream_terminated.load());
        assert(elapsed_ms < 1000); // Full stream would have taken 3000ms
        assert(disconnect_chunks_sent.load() < 15);
    }

    // 4. Top-level stream parsing precision: nested "stream":true is ignored; non-boolean top-level "stream" -> 400
    {
        upstream_post_calls.store(0);
        auto res_nested = perform_http(
            "POST",
            gateway_url,
            R"({"model":"mock/nonstream","messages":[{"role":"user","content":"\"stream\":true"}],"metadata":{"stream":true},"stream":false})",
            {"Content-Type: application/json"}
        );
        assert(res_nested.status == 200);
        assert(upstream_post_calls.load() == 1);
        assert(has_header_containing(res_nested.headers, "application/json"));

        upstream_post_calls.store(0);
        auto res_bad_stream = perform_http(
            "POST",
            gateway_url,
            R"({"model":"mock/nonstream","messages":[],"stream":"true"})",
            {"Content-Type: application/json"}
        );
        assert(res_bad_stream.status == 400);
        assert(upstream_post_calls.load() == 0);
        assert(res_bad_stream.body.find("bad_request") != std::string::npos);
        assert(shutdown_coordinator.active_requests() == 0);
    }

    server.stop();
    if (server_thread.joinable()) server_thread.join();
    mock_upstream.stop();
    if (mock_thread.joinable()) mock_thread.join();

    std::cout << "[PASS] H-01 SSE streaming & upstream proxy regression tests.\n";
}

void test_h02_request_counter_and_lifecycle() {
    // 1. Direct ShutdownCoordinator invariants
    {
        gateway::ShutdownCoordinator coord;
        assert(coord.active_requests() == 0);
        coord.leave_request(); // Should not underflow below 0
        assert(coord.active_requests() == 0);

        {
            auto g1 = coord.acquire_guard();
            assert(static_cast<bool>(g1));
            assert(coord.active_requests() == 1);
            auto g2 = std::move(g1);
            assert(!static_cast<bool>(g1));
            assert(static_cast<bool>(g2));
            assert(coord.active_requests() == 1);
        }
        assert(coord.active_requests() == 0);
    }

    // 2. HttpServer active_requests() invariance across rejected/OPTIONS/error/SSE paths + graceful shutdown
    httplib::Server mock_upstream;
    std::atomic<bool> slow_sse_first_chunk_sent{false};

    mock_upstream.Post("/v1/chat/completions", [&](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream",
            [&](size_t /*offset*/, httplib::DataSink& sink) {
                const char* c1 = "data: {\"step\":1}\n\n";
                if (!sink.write(c1, std::strlen(c1))) return false;
                slow_sse_first_chunk_sent.store(true);
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const char* c2 = "data: [DONE]\n\n";
                if (!sink.write(c2, std::strlen(c2))) return false;
                sink.done();
                return true;
            }
        );
    });

    int mock_port = mock_upstream.bind_to_any_port("127.0.0.1");
    assert(mock_port > 0);
    std::thread mock_thread([&]() { mock_upstream.listen_after_bind(); });

    gateway::GatewayConfig config;
    config.listen.host = "127.0.0.1";
    config.listen.port = 18768;
    config.embedded_engine.enabled = false;
    config.limits.max_request_bytes = 256;
    config.security.client_auth = false;
    config.security.cors_enabled = true;
    config.security.allowed_origins = {"http://trusted.local:5173"};

    gateway::ProviderConfig prov;
    prov.id = "mock";
    prov.base_url = "http://127.0.0.1:" + std::to_string(mock_port) + "/v1";
    prov.authentication = gateway::Authentication::None;
    prov.allowlist = {"mock/*"};
    config.providers.push_back(prov);

    gateway::CredentialStore store("LLMGatewayPhase1H02Test_Synthetic", false);
    gateway::EmbeddedEngine embedded_engine(config, true);
    gateway::ShutdownCoordinator shutdown_coordinator;
    gateway::HttpServer server(config, store, embedded_engine, shutdown_coordinator);

    std::atomic<bool> shutdown_action_invoked{false};
    std::chrono::steady_clock::time_point shutdown_invoked_at{};
    shutdown_coordinator.set_shutdown_action([&]() {
        shutdown_invoked_at = std::chrono::steady_clock::now();
        shutdown_action_invoked.store(true);
        server.stop();
    });

    std::thread server_thread([&server]() { server.start(); });
    server.wait_until_ready();

    const std::string base_url = "http://127.0.0.1:18768";

    // (a) Host mismatch -> 403
    auto r_host = perform_http("GET", base_url + "/health", "", {"Host: evil.example:18768"});
    assert(r_host.status == 403);
    assert(shutdown_coordinator.active_requests() == 0);

    // (b) Disallowed CORS Origin -> 403
    auto r_cors_bad = perform_http("GET", base_url + "/health", "", {"Origin: http://evil.example"});
    assert(r_cors_bad.status == 403);
    assert(shutdown_coordinator.active_requests() == 0);

    // (c) Valid CORS Preflight OPTIONS -> 204
    auto r_opt = perform_http(
        "OPTIONS",
        base_url + "/v1/chat/completions",
        "",
        {
            "Origin: http://trusted.local:5173",
            "Access-Control-Request-Method: POST",
            "Access-Control-Request-Headers: content-type, authorization"
        }
    );
    assert(r_opt.status == 204);
    assert(shutdown_coordinator.active_requests() == 0);

    // (d) Oversized body -> 413
    std::string huge_body(512, 'x');
    auto r_large = perform_http("POST", base_url + "/v1/chat/completions", huge_body, {"Content-Type: application/json"});
    assert(r_large.status == 413);
    assert(shutdown_coordinator.active_requests() == 0);

    // (e) Unknown route -> 404
    auto r_404 = perform_http("GET", base_url + "/unknown-endpoint");
    assert(r_404.status == 404);
    assert(shutdown_coordinator.active_requests() == 0);

    // (f) Invalid JSON -> 400
    auto r_400 = perform_http("POST", base_url + "/v1/chat/completions", "{invalid_json", {"Content-Type: application/json"});
    assert(r_400.status == 400);
    assert(shutdown_coordinator.active_requests() == 0);

    // (g) Graceful shutdown waits for in-flight SSE stream and rejects new requests with 503
    std::chrono::steady_clock::time_point sse_completed_at{};
    std::thread sse_client([&]() {
        auto res = perform_http(
            "POST",
            base_url + "/v1/chat/completions",
            R"({"model":"mock/slow","messages":[],"stream":true})",
            {"Content-Type: application/json"}
        );
        sse_completed_at = std::chrono::steady_clock::now();
        assert(res.status == 200);
        assert(res.body.find("data: [DONE]\n\n") != std::string::npos);
    });

    while (!slow_sse_first_chunk_sent.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(shutdown_coordinator.active_requests() == 1);

    // Trigger graceful shutdown while SSE stream is active
    shutdown_coordinator.trigger_shutdown(std::chrono::milliseconds(2000));
    assert(shutdown_coordinator.is_shutting_down());

    // New request during shutdown must receive 503 and must NOT decrement the active SSE request count
    auto r_503 = perform_http("GET", base_url + "/health");
    assert(r_503.status == 503);
    assert(shutdown_coordinator.active_requests() == 1);

    sse_client.join();
    if (server_thread.joinable()) server_thread.join();

    assert(shutdown_action_invoked.load());
    assert(shutdown_coordinator.active_requests() == 0);
    // Shutdown action must not have fired before the SSE stream finished draining
    auto drain_diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        shutdown_invoked_at - sse_completed_at).count();
    assert(drain_diff_ms >= -50);

    mock_upstream.stop();
    if (mock_thread.joinable()) mock_thread.join();

    // 3. ProcessController cooperative cancellation test
    {
        store.set_credential("llm_gateway|admin", "synth-cancel-admin-token");
        httplib::Server stubborn_health;
        stubborn_health.Get("/health", [](const httplib::Request&, httplib::Response& res) {
            res.set_content(R"({"status":"ok","version":"2.0.0","embedded":"disabled","pid":99999})", "application/json");
        });
        stubborn_health.Post("/admin/shutdown", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(R"({"status":"shutting_down"})", "application/json");
            // Intentionally keep listening so port release wait can be cancelled
        });
        int stubborn_port = stubborn_health.bind_to_any_port("127.0.0.1");
        assert(stubborn_port > 0);
        std::thread stubborn_thread([&]() { stubborn_health.listen_after_bind(); });

        auto tmp_cfg_path = std::filesystem::temp_directory_path() / "llm_gateway_h02_cancel_test_synth.json";
        gateway::GatewayConfig cancel_cfg;
        cancel_cfg.listen.host = "127.0.0.1";
        cancel_cfg.listen.port = static_cast<uint16_t>(stubborn_port);
        cancel_cfg.embedded_engine.enabled = false;

        maintenance::ProcessController proc_ctrl("nonexistent-gateway.exe", store);
        std::string restart_err;
        auto t0 = std::chrono::steady_clock::now();

        std::thread cancel_trigger([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            proc_ctrl.request_cancel();
        });

        bool restarted = proc_ctrl.restart_pipeline(cancel_cfg, tmp_cfg_path, nullptr, &restart_err);
        auto cancel_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        cancel_trigger.join();
        stubborn_health.stop();
        if (stubborn_thread.joinable()) stubborn_thread.join();

        store.delete_credential("llm_gateway|admin");
        std::error_code ec;
        std::filesystem::remove(tmp_cfg_path, ec);

        assert(!restarted);
        assert(restart_err.find("cancelled") != std::string::npos);
        assert(cancel_elapsed_ms < 1500); // Cancelled promptly instead of waiting 5000ms for port release
    }

    std::cout << "[PASS] H-02 request counter, graceful shutdown & ProcessController cancellation tests.\n";
}

void test_phase2_h03_h05_h06_h07_h11_h12_h14() {
    // 1. H-05: HealthClient strict schema validation
    {
        assert(!maintenance::HealthClient::parse_health_payload("").has_value());
        assert(!maintenance::HealthClient::parse_health_payload("{}").has_value());
        assert(!maintenance::HealthClient::parse_health_payload(R"({"status":"ok"})").has_value());
        assert(!maintenance::HealthClient::parse_health_payload(R"({"status":"error","pid":100,"version":"2.0.0","embedded":"ready"})").has_value());
        assert(!maintenance::HealthClient::parse_health_payload(R"({"status":"ok","pid":0,"version":"2.0.0","embedded":"ready"})").has_value());
        assert(!maintenance::HealthClient::parse_health_payload(R"({"status":"ok","pid":100,"version":"","embedded":"ready"})").has_value());
        assert(!maintenance::HealthClient::parse_health_payload(R"({"status":"ok","pid":100,"version":"2.0.0","embedded":"bogus"})").has_value());

        auto valid = maintenance::HealthClient::parse_health_payload(R"({"status":"ok","pid":4321,"version":"2.0.0","embedded":"error"})");
        assert(valid.has_value());
        assert(valid->pid == 4321);
        assert(valid->embedded == "error");
    }

    // 2. H-03 (Embedded Fault Isolation), H-05 (Latency & Logger), and H-12 (HTTP API Strict Validation)
    httplib::Server mock_upstream;
    mock_upstream.Post("/v1/chat/completions", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content(R"({"id":"chatcmpl-cloud-ok","object":"chat.completion"})", "application/json");
    });
    mock_upstream.Get("/v1/models", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content(R"({"object":"list","data":[{"id":"gpt-mock"}]})", "application/json");
    });
    int mock_port = mock_upstream.bind_to_any_port("127.0.0.1");
    assert(mock_port > 0);
    std::thread mock_thread([&]() { mock_upstream.listen_after_bind(); });

    std::error_code ec;
    auto log_dir = std::filesystem::temp_directory_path() / gateway::path_from_utf8("llm_gw_phase2_logs_設定 🧪");
    std::filesystem::remove_all(log_dir, ec);
    gateway::get_logger().init(gateway::path_to_utf8(log_dir), "INFO");

    gateway::GatewayConfig config;
    config.listen.host = "127.0.0.1";
    config.listen.port = 18769;
    config.security.client_auth = false;
    config.embedded_engine.enabled = true;
    gateway::ModelFileConfig missing_emb;
    missing_emb.path = "nonexistent_phase2_model_🧪.gguf";
    missing_emb.n_threads = 1;
    config.embedded_engine.embedding_model = missing_emb;
    config.embedded_engine.reranker_model = missing_emb;

    gateway::ProviderConfig emb_prov;
    emb_prov.id = "embedded";
    emb_prov.authentication = gateway::Authentication::None;
    emb_prov.allowlist = {"embedded/*"};
    config.providers.push_back(emb_prov);

    gateway::ProviderConfig cloud_prov;
    cloud_prov.id = "mock";
    cloud_prov.base_url = "http://127.0.0.1:" + std::to_string(mock_port) + "/v1";
    cloud_prov.authentication = gateway::Authentication::None;
    cloud_prov.model_discovery = true;
    cloud_prov.allowlist = {"mock/*"};
    config.providers.push_back(cloud_prov);

    gateway::CredentialStore store("LLMGatewayPhase2Test_Synthetic", false);
    gateway::EmbeddedEngine embedded_engine(config, false);
    gateway::ShutdownCoordinator shutdown_coordinator;
    gateway::HttpServer server(config, store, embedded_engine, shutdown_coordinator);

    embedded_engine.start_async_load();

    std::thread server_thread([&server]() { server.start(); });
    server.wait_until_ready();

    maintenance::HealthClient health_client;
    std::optional<maintenance::HealthResponse> health_opt;
    for (int i = 0; i < 30; ++i) {
        health_opt = health_client.check_health(18769, 500);
        if (health_opt && health_opt->embedded == "error") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    assert(health_opt.has_value());
    assert(health_opt->status == "ok");
    assert(health_opt->embedded == "error");

    const std::string base_url = "http://127.0.0.1:18769";

    // H-03: External cloud routes work normally even when embedded engine is in Error state
    {
        auto r_chat = perform_http(
            "POST",
            base_url + "/v1/chat/completions",
            R"({"model":"mock/gpt-mock","messages":[{"role":"user","content":"hello"}]})",
            {"Content-Type: application/json"}
        );
        assert(r_chat.status == 200);
        assert(r_chat.body.find("chatcmpl-cloud-ok") != std::string::npos);

        auto r_models = perform_http("GET", base_url + "/v1/models");
        assert(r_models.status == 200);
        assert(r_models.body.find("mock/gpt-mock") != std::string::npos);

        // Embedded route returns 503 embedded_load_failed (isolated failure)
        auto r_emb = perform_http(
            "POST",
            base_url + "/v1/embeddings",
            R"({"model":"embedded/embedding_model","input":"test"})",
            {"Content-Type: application/json"}
        );
        assert(r_emb.status == 503);
        assert(r_emb.body.find("embedded_load_failed") != std::string::npos);
    }

    // H-12: Strict type & range validation on /v1/embeddings and /v1/rerank
    {
        // Non-string encoding_format -> 400 bad_request (not 500!)
        auto r_fmt = perform_http(
            "POST",
            base_url + "/v1/embeddings",
            R"({"model":"embedded/embedding_model","input":"test","encoding_format":123})",
            {"Content-Type: application/json"}
        );
        assert(r_fmt.status == 400);
        assert(r_fmt.body.find("bad_request") != std::string::npos);

        // Float or non-positive dimensions -> 400 bad_request
        auto r_dim_float = perform_http(
            "POST",
            base_url + "/v1/embeddings",
            R"({"model":"embedded/embedding_model","input":"test","dimensions":128.5})",
            {"Content-Type: application/json"}
        );
        assert(r_dim_float.status == 400);

        auto r_dim_zero = perform_http(
            "POST",
            base_url + "/v1/embeddings",
            R"({"model":"embedded/embedding_model","input":"test","dimensions":0})",
            {"Content-Type: application/json"}
        );
        assert(r_dim_zero.status == 400);

        // Float top_n or non-boolean return_documents on /v1/rerank -> 400 bad_request
        auto r_rerank_topn = perform_http(
            "POST",
            base_url + "/v1/rerank",
            R"({"model":"embedded/reranker_model","query":"q","documents":["d1","d2"],"top_n":1.5})",
            {"Content-Type: application/json"}
        );
        assert(r_rerank_topn.status == 400);

        auto r_rerank_retdoc = perform_http(
            "POST",
            base_url + "/v1/rerank",
            R"({"model":"embedded/reranker_model","query":"q","documents":["d1","d2"],"return_documents":"yes"})",
            {"Content-Type: application/json"}
        );
        assert(r_rerank_retdoc.status == 400);
    }

    server.stop();
    if (server_thread.joinable()) server_thread.join();
    mock_upstream.stop();
    if (mock_thread.joinable()) mock_thread.join();

    // Verify H-05 logger wrote to Unicode/emoji directory with latency_ms= and 0 write errors
    assert(gateway::get_logger().write_error_count() == 0);
    bool found_latency_log = false;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir, ec)) {
        if (entry.is_regular_file()) {
            std::ifstream ifs(entry.path());
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (content.find("latency_ms=") != std::string::npos) {
                found_latency_log = true;
            }
        }
    }
    assert(found_latency_log);

    // Verify H-05 logger write error detection when pointed to a directory as fixed_file
    gateway::get_logger().init(gateway::path_to_utf8(log_dir), "INFO", gateway::path_to_utf8(log_dir));
    gateway::get_logger().info("This write must fail because target is a directory");
    assert(gateway::get_logger().write_error_count() > 0);
    assert(!gateway::get_logger().last_write_error().empty());
    std::filesystem::remove_all(log_dir, ec);

    // 3. H-06 & H-14: ConfigStore::save_atomic pre-save validation & Unicode/emoji path atomic save
    {
        auto cfg_dir = std::filesystem::temp_directory_path() / gateway::path_from_utf8("llm_gw_cfgstore_設定 🧪");
        std::filesystem::remove_all(cfg_dir, ec);
        auto cfg_path = cfg_dir / gateway::path_from_utf8("設定 🧪.json");

        maintenance::ConfigStore cfg_store(cfg_path);
        gateway::GatewayConfig valid_cfg;
        valid_cfg.listen.port = 18888;
        std::string save_err;
        assert(cfg_store.save_atomic(valid_cfg, &save_err));
        assert(save_err.empty());

        auto loaded_cfg = cfg_store.load();
        assert(loaded_cfg.listen.port == 18888);

        // Invalid config (port < 1024) must be rejected BEFORE touching the existing valid file on disk
        gateway::GatewayConfig invalid_cfg = valid_cfg;
        invalid_cfg.listen.port = 80;
        std::string invalid_err;
        assert(!cfg_store.save_atomic(invalid_cfg, &invalid_err));
        assert(!invalid_err.empty());

        // Existing file on disk must still have port 18888
        auto still_valid = cfg_store.load();
        assert(still_valid.listen.port == 18888);

        std::filesystem::remove_all(cfg_dir, ec);
    }

    // 4. H-07: ProcessController::stop_gateway custom admin_credential_name, HTTP status check & port verification
    {
        const std::string custom_admin_cred = "llm_gateway|custom_admin_phase2_synth";
        store.set_credential(custom_admin_cred, "phase2-custom-admin-secret");

        std::atomic<bool> allow_shutdown{false};
        httplib::Server mock_admin_srv;
        mock_admin_srv.Post("/admin/shutdown", [&](const httplib::Request& req, httplib::Response& res) {
            auto auth = req.get_header_value("Authorization");
            if (!allow_shutdown.load() || auth != "Bearer phase2-custom-admin-secret") {
                res.status = 401;
                res.set_content(R"({"error":{"code":"unauthorized"}})", "application/json");
                return;
            }
            res.status = 200;
            res.set_content(R"({"status":"shutting_down"})", "application/json");
            std::thread([&mock_admin_srv]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                mock_admin_srv.stop();
            }).detach();
        });

        int admin_port = mock_admin_srv.bind_to_any_port("127.0.0.1");
        assert(admin_port > 0);
        std::thread admin_thread([&]() { mock_admin_srv.listen_after_bind(); });

        maintenance::ProcessController proc_ctrl("nonexistent-gateway.exe", store);

        // (a) When /admin/shutdown returns 401 and no process handle is attached, stop_gateway must return false!
        std::string stop_err;
        bool stopped_401 = proc_ctrl.stop_gateway(static_cast<uint16_t>(admin_port), &stop_err, custom_admin_cred);
        assert(!stopped_401);
        assert(stop_err.find("401") != std::string::npos);

        // (b) When /admin/shutdown accepts the custom admin credential (200 OK) and releases the port, stop_gateway returns true!
        allow_shutdown.store(true);
        stop_err.clear();
        bool stopped_200 = proc_ctrl.stop_gateway(static_cast<uint16_t>(admin_port), &stop_err, custom_admin_cred);
        assert(stopped_200);

        mock_admin_srv.stop();
        if (admin_thread.joinable()) admin_thread.join();
        store.delete_credential(custom_admin_cred);
    }

    // 5. H-11: CredentialEditor token generation failure & empty clipboard rejection
    {
        maintenance::CredentialEditor editor(store);
        std::string failed_tok = editor.generate_and_store_token("");
        assert(failed_tok.empty());
        assert(!maintenance::CredentialEditor::copy_to_clipboard_temporary("", 0));
    }

    // 6. H-05 & H-14: Subprocess startup failure diagnostic capture with Unicode/emoji config path
    {
        wchar_t self_buf[MAX_PATH];
        GetModuleFileNameW(nullptr, self_buf, MAX_PATH);
        std::filesystem::path gw_exe = std::filesystem::path(self_buf).parent_path() / "llm-gateway.exe";
        if (std::filesystem::exists(gw_exe, ec)) {
            auto bad_dir = std::filesystem::temp_directory_path() / gateway::path_from_utf8("llm_gw_bad_startup_設定 🧪");
            std::filesystem::create_directories(bad_dir, ec);
            auto bad_cfg_path = bad_dir / gateway::path_from_utf8("壊れた設定 🧪.json");
            {
                std::ofstream ofs(bad_cfg_path, std::ios::binary);
                ofs << "{broken_json";
            }
            maintenance::ProcessController real_ctrl(gw_exe, store);
            std::string start_err;
            bool started = real_ctrl.start_gateway(bad_cfg_path, &start_err);
            assert(!started);
            assert(start_err.find("exit_code=1") != std::string::npos);
            assert(start_err.find("壊れた設定 🧪.json") != std::string::npos);
            std::filesystem::remove_all(bad_dir, ec);
        }
    }

    std::cout << "[PASS] Phase 2 (H-03, H-05, H-06, H-07, H-11, H-12, H-14) regression tests.\n";
}

void test_phase3_h04_h08() {
    std::error_code ec;

    // =========================================================================
    // Part A: H-08 Embedded Inference End-to-End & Boundary Tests via HttpServer
    // =========================================================================
    {
        auto log_dir = std::filesystem::temp_directory_path() / "llm_gw_phase3_h08_logs";
        std::filesystem::create_directories(log_dir, ec);
        gateway::get_logger().init(gateway::path_to_utf8(log_dir), "INFO");

        gateway::GatewayConfig config;
        config.listen.host = "127.0.0.1";
        config.listen.port = 18775;
        config.security.client_auth = false;
        config.embedded_engine.enabled = true;
        config.embedded_engine.embedding_model = gateway::ModelFileConfig{"synthetic_emb.gguf", 2, 0};
        config.embedded_engine.reranker_model = gateway::ModelFileConfig{"synthetic_rerank.gguf", 2, 0};

        gateway::ProviderConfig emb_prov;
        emb_prov.id = "embedded";
        emb_prov.authentication = gateway::Authentication::None;
        emb_prov.allowlist = {"embedded/*"};
        config.providers.push_back(emb_prov);

        config.models.push_back(gateway::ModelAlias{"local-embedding", "embedded", "synthetic_emb.gguf", std::nullopt});
        config.models.push_back(gateway::ModelAlias{"local-reranker", "embedded", "synthetic_rerank.gguf", std::nullopt});

        gateway::CredentialStore store("TestLLMGateway_Phase3_H08_NotUsed", false);
        gateway::EmbeddedEngine embedded(config, false);
        gateway::ShutdownCoordinator coordinator;

        // Deterministic synthetic embedding & rerank hooks (4D vectors, max_tokens = 16)
        std::atomic<bool> inject_nan{false};
        std::atomic<bool> inject_null{false};

        embedded.embedding().enable_test_mode(
            "synthetic_emb.gguf",
            4,
            16,
            [&](const std::string& text) -> std::optional<std::vector<float>> {
                if (inject_null.load()) {
                    return std::nullopt;
                }
                if (inject_nan.load()) {
                    return std::vector<float>{1.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f, 0.25f};
                }
                // Deterministic 4D vector derived solely from current input text
                float h0 = static_cast<float>(text.size());
                float h1 = 0.0f;
                float h2 = 0.0f;
                for (size_t i = 0; i < text.size(); ++i) {
                    h1 += static_cast<float>(static_cast<unsigned char>(text[i])) * static_cast<float>(i + 1);
                    h2 += static_cast<float>(static_cast<unsigned char>(text[i]));
                }
                return std::vector<float>{h0, h1, h2, 1.0f};
            });

        embedded.reranker().enable_test_mode(
            "synthetic_rerank.gguf",
            16,
            [&](const std::string& query, const std::string& doc) -> std::optional<float> {
                if (inject_null.load()) {
                    return std::nullopt;
                }
                if (inject_nan.load()) {
                    return std::numeric_limits<float>::quiet_NaN();
                }
                // Deterministic raw logit depending solely on (query, doc) pair (no cross-document leakage)
                if (doc == "doc_counter_0") return 0.0f;
                if (doc == "doc_counter_1") return 3.0e-6f;
                if (doc == "doc_counter_2") return 6.0e-6f;
                float s = 0.0f;
                for (unsigned char c : query) s += static_cast<float>(c) * 0.001f;
                for (size_t i = 0; i < doc.size(); ++i) {
                    s += static_cast<float>(static_cast<unsigned char>(doc[i])) * static_cast<float>(i + 1) * 0.01f;
                }
                return s;
            });

        gateway::HttpServer server(config, store, embedded, coordinator);
        std::thread server_thread([&]() { server.start(); });
        server.wait_until_ready();

        const std::string emb_url = "http://127.0.0.1:18775/v1/embeddings";
        const std::string rerank_url = "http://127.0.0.1:18775/v1/rerank";
        const std::vector<std::string> json_hdr = {"Content-Type: application/json"};

        // 1. Embeddings A -> B -> A consistency & batch input consistency
        auto emb_a1 = perform_http("POST", emb_url, R"({"model":"local-embedding","input":"alpha"})", json_hdr);
        auto emb_b  = perform_http("POST", emb_url, R"({"model":"local-embedding","input":"beta_text"})", json_hdr);
        auto emb_a2 = perform_http("POST", emb_url, R"({"model":"local-embedding","input":"alpha"})", json_hdr);
        assert(emb_a1.status == 200);
        assert(emb_b.status == 200);
        assert(emb_a2.status == 200);
        auto j_emb_a1 = nlohmann::json::parse(emb_a1.body);
        auto j_emb_b  = nlohmann::json::parse(emb_b.body);
        auto j_emb_a2 = nlohmann::json::parse(emb_a2.body);
        assert(j_emb_a1["data"][0]["embedding"] == j_emb_a2["data"][0]["embedding"]);
        assert(j_emb_a1["data"][0]["embedding"] != j_emb_b["data"][0]["embedding"]);

        auto emb_batch = perform_http("POST", emb_url, R"({"model":"local-embedding","input":["alpha","beta_text","alpha"]})", json_hdr);
        assert(emb_batch.status == 200);
        auto j_batch = nlohmann::json::parse(emb_batch.body);
        assert(j_batch["data"].size() == 3);
        assert(j_batch["data"][0]["embedding"] == j_emb_a1["data"][0]["embedding"]);
        assert(j_batch["data"][1]["embedding"] == j_emb_b["data"][0]["embedding"]);
        assert(j_batch["data"][2]["embedding"] == j_emb_a1["data"][0]["embedding"]);

        // 2. Rerank A -> B -> A consistency, document permutation invariance & close-score sorting
        auto rr_perm1 = perform_http("POST", rerank_url,
            R"({"model":"local-reranker","query":"q","documents":["doc_counter_0","doc_counter_1","doc_counter_2"]})", json_hdr);
        auto rr_other = perform_http("POST", rerank_url,
            R"({"model":"local-reranker","query":"other","documents":["short","longer_doc"]})", json_hdr);
        auto rr_perm2 = perform_http("POST", rerank_url,
            R"({"model":"local-reranker","query":"q","documents":["doc_counter_2","doc_counter_0","doc_counter_1"]})", json_hdr);
        auto rr_perm1_repeat = perform_http("POST", rerank_url,
            R"({"model":"local-reranker","query":"q","documents":["doc_counter_0","doc_counter_1","doc_counter_2"]})", json_hdr);

        assert(rr_perm1.status == 200);
        assert(rr_other.status == 200);
        assert(rr_perm2.status == 200);
        assert(rr_perm1_repeat.status == 200);

        auto j_rr1 = nlohmann::json::parse(rr_perm1.body);
        auto j_rr2 = nlohmann::json::parse(rr_perm2.body);
        auto j_rr1_rep = nlohmann::json::parse(rr_perm1_repeat.body);

        // Consecutive A -> B -> A must match identically
        assert(j_rr1["results"] == j_rr1_rep["results"]);
        // Highest score is doc_counter_2 (logit 6e-6 -> score ~0.5000015), then doc_counter_1 (~0.50000075), then doc_counter_0 (0.5)
        assert(j_rr1["results"][0]["index"] == 2);
        assert(j_rr1["results"][1]["index"] == 1);
        assert(j_rr1["results"][2]["index"] == 0);
        // In permuted input ["doc_counter_2", "doc_counter_0", "doc_counter_1"], indices are 0, 2, 1 with identical top scores
        assert(j_rr2["results"][0]["index"] == 0);
        assert(j_rr2["results"][1]["index"] == 2);
        assert(j_rr2["results"][2]["index"] == 1);
        assert(j_rr1["results"][0]["relevance_score"] == j_rr2["results"][0]["relevance_score"]);
        assert(j_rr1["results"][1]["relevance_score"] == j_rr2["results"][1]["relevance_score"]);
        assert(j_rr1["results"][2]["relevance_score"] == j_rr2["results"][2]["relevance_score"]);

        // 3. Token limit exceeded (> 16 tokens) -> 400 bad_request
        std::string long_text(100, 'a');
        auto emb_too_long = perform_http("POST", emb_url,
            R"({"model":"local-embedding","input":")" + long_text + R"("})", json_hdr);
        assert(emb_too_long.status == 400);
        assert(nlohmann::json::parse(emb_too_long.body)["error"]["code"] == "bad_request");

        auto rr_too_long = perform_http("POST", rerank_url,
            R"({"model":"local-reranker","query":"long_query","documents":[")" + long_text + R"("]})", json_hdr);
        assert(rr_too_long.status == 400);
        assert(nlohmann::json::parse(rr_too_long.body)["error"]["code"] == "bad_request");

        // 4. Empty document in /v1/rerank -> 400 bad_request
        auto rr_empty_doc = perform_http("POST", rerank_url,
            R"({"model":"local-reranker","query":"q","documents":["valid_doc",""]})", json_hdr);
        assert(rr_empty_doc.status == 400);
        assert(nlohmann::json::parse(rr_empty_doc.body)["error"]["code"] == "bad_request");

        // 5. Extraction failure (nullopt or NaN) -> 503 service_unavailable (never silent score=0 200 OK)
        inject_null.store(true);
        auto emb_null = perform_http("POST", emb_url, R"({"model":"local-embedding","input":"alpha"})", json_hdr);
        auto rr_null  = perform_http("POST", rerank_url, R"({"model":"local-reranker","query":"q","documents":["doc1"]})", json_hdr);
        assert(emb_null.status == 503);
        assert(rr_null.status == 503);
        assert(nlohmann::json::parse(emb_null.body)["error"]["code"] == "embedded_load_failed");
        assert(nlohmann::json::parse(rr_null.body)["error"]["code"] == "embedded_load_failed");
        inject_null.store(false);

        inject_nan.store(true);
        auto emb_nan = perform_http("POST", emb_url, R"({"model":"local-embedding","input":"alpha"})", json_hdr);
        auto rr_nan  = perform_http("POST", rerank_url, R"({"model":"local-reranker","query":"q","documents":["doc1"]})", json_hdr);
        assert(emb_nan.status == 503);
        assert(rr_nan.status == 503);
        inject_nan.store(false);

        // 6. Concurrent multi-threaded requests to /v1/embeddings and /v1/rerank
        std::vector<std::thread> workers;
        std::atomic<int> ok_count{0};
        for (int t = 0; t < 6; ++t) {
            workers.emplace_back([&, t]() {
                if (t % 2 == 0) {
                    auto r = perform_http("POST", emb_url, R"({"model":"local-embedding","input":"alpha"})", json_hdr);
                    if (r.status == 200) {
                        auto j = nlohmann::json::parse(r.body);
                        if (j["data"][0]["embedding"] == j_emb_a1["data"][0]["embedding"]) {
                            ok_count.fetch_add(1);
                        }
                    }
                } else {
                    auto r = perform_http("POST", rerank_url,
                        R"({"model":"local-reranker","query":"q","documents":["doc_counter_0","doc_counter_1","doc_counter_2"]})", json_hdr);
                    if (r.status == 200) {
                        auto j = nlohmann::json::parse(r.body);
                        if (j["results"] == j_rr1["results"]) {
                            ok_count.fetch_add(1);
                        }
                    }
                }
            });
        }
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        assert(ok_count.load() == 6);

        server.stop();
        if (server_thread.joinable()) server_thread.join();
        std::filesystem::remove_all(log_dir, ec);
    }

    // =========================================================================
    // Part B: H-04 GUI Initial Setup ConfigStore & ProcessController Tests
    // =========================================================================
    {
        auto temp_dir = std::filesystem::temp_directory_path() / gateway::path_from_utf8("llm_gw_phase3_h04_設定 🧪");
        std::filesystem::remove_all(temp_dir, ec);
        std::filesystem::create_directories(temp_dir, ec);

        // 1. Start from a non-existent config file path (first-run initial setup)
        auto new_cfg_path = temp_dir / gateway::path_from_utf8("新規設定_v2.json");
        maintenance::ConfigStore store(new_cfg_path);
        assert(!store.exists_on_disk());

        gateway::GatewayConfig cfg = store.load();
        assert(!store.exists_on_disk());
        assert(!cfg.loaded_from_legacy);
        gateway::GatewayConfig initial_snapshot = cfg;
        assert(maintenance::ConfigStore::configs_equal(cfg, initial_snapshot));

        // 2. Apply OpenRouter and LM Studio presets via upsert_provider
        std::string err;
        auto or_preset = maintenance::ConfigStore::make_openrouter_preset();
        assert(maintenance::ConfigStore::upsert_provider(cfg, or_preset, &err));
        auto lms_preset = maintenance::ConfigStore::make_lm_studio_preset();
        assert(maintenance::ConfigStore::upsert_provider(cfg, lms_preset, &err));
        assert(cfg.find_provider("openrouter") != nullptr);
        assert(cfg.find_provider("lm-studio") != nullptr);
        assert(!maintenance::ConfigStore::configs_equal(cfg, initial_snapshot));

        // Verify provider validation rejects invalid URL or missing key_vendor/key_slot on bearer provider
        gateway::ProviderConfig bad_prov;
        bad_prov.id = "bad-prov";
        bad_prov.base_url = "ftp://invalid.example.com";
        bad_prov.authentication = gateway::Authentication::None;
        assert(!maintenance::ConfigStore::upsert_provider(cfg, bad_prov, &err));

        bad_prov.base_url = "https://valid.example.com/v1";
        bad_prov.authentication = gateway::Authentication::Bearer;
        bad_prov.key_vendor = ""; // missing key_vendor for Bearer
        bad_prov.key_slot = "";
        assert(!maintenance::ConfigStore::upsert_provider(cfg, bad_prov, &err));

        // 3. Add Model Aliases & verify provider reference validation
        gateway::ModelAlias bad_alias{"bad-alias", "nonexistent-provider", "gpt-4o", std::nullopt};
        assert(!maintenance::ConfigStore::upsert_model_alias(cfg, bad_alias, &err));

        gateway::ModelAlias coding_alias{"default-coding", "openrouter", "openai/gpt-4o", std::nullopt};
        assert(maintenance::ConfigStore::upsert_model_alias(cfg, coding_alias, &err));
        gateway::ModelAlias local_alias{"local-chat", "lm-studio", "qwen2.5-coder-7b", std::nullopt};
        assert(maintenance::ConfigStore::upsert_model_alias(cfg, local_alias, &err));
        assert(cfg.find_alias("default-coding") != nullptr);
        assert(cfg.find_alias("local-chat") != nullptr);

        // 4. Verify remove_provider blocks deletion while referenced by an alias, and succeeds after alias removal
        assert(!maintenance::ConfigStore::remove_provider(cfg, "openrouter", &err));
        assert(err.find("default-coding") != std::string::npos);

        assert(maintenance::ConfigStore::remove_model_alias(cfg, "default-coding", &err));
        assert(maintenance::ConfigStore::remove_provider(cfg, "openrouter", &err));
        assert(cfg.find_provider("openrouter") == nullptr);

        // Re-add OpenRouter preset & alias for complete initial setup save
        assert(maintenance::ConfigStore::upsert_provider(cfg, or_preset, &err));
        assert(maintenance::ConfigStore::upsert_model_alias(cfg, coding_alias, &err));

        // 5. Edit Gateway listen port, client auth, CORS origins, and Embedded GGUF paths
        cfg.listen.port = 18799;
        cfg.security.client_auth = true;
        cfg.security.credential_name = "llm_gateway|default";
        cfg.security.admin_credential_name = "llm_gateway|admin";
        cfg.security.allowed_origins = maintenance::ConfigStore::split_csv_tokens(
            "http://localhost:5173, http://127.0.0.1:3000");
        assert(cfg.security.allowed_origins.size() == 2);
        assert(maintenance::ConfigStore::join_csv_tokens(cfg.security.allowed_origins) ==
               "http://localhost:5173, http://127.0.0.1:3000");

        cfg.embedded_engine.enabled = true;
        cfg.embedded_engine.embedding_model = gateway::ModelFileConfig{"models/bge-small-en-v1.5.gguf", 0, 0};
        cfg.embedded_engine.reranker_model = gateway::ModelFileConfig{"models/bge-reranker-v2-m3.gguf", 0, 0};

        assert(maintenance::ConfigStore::validate_config(cfg, &err));
        assert(store.save_atomic(cfg, &err));
        assert(store.exists_on_disk());

        // Reload from disk and verify equality
        gateway::GatewayConfig reloaded = store.load();
        assert(maintenance::ConfigStore::configs_equal(cfg, reloaded));
        assert(reloaded.listen.port == 18799);
        assert(reloaded.find_provider("openrouter") != nullptr);
        assert(reloaded.find_provider("lm-studio") != nullptr);
        assert(reloaded.find_alias("default-coding") != nullptr);
        assert(reloaded.find_alias("local-chat") != nullptr);
        assert(reloaded.embedded_engine.enabled);
        assert(reloaded.embedded_engine.embedding_model.has_value());
        assert(reloaded.embedded_engine.reranker_model.has_value());

        // 6. Legacy v1.0 config "Save Copy As" non-destructive workflow
        auto legacy_path = temp_dir / "legacy_v1.json";
        const std::string legacy_raw = R"({"listen":{"address":"127.0.0.1","port":8811},"proxy":{"connect_timeout_sec":5,"read_timeout_sec":30},"providers":{}})";
        {
            std::ofstream ofs(legacy_path, std::ios::binary);
            ofs << legacy_raw;
        }
        maintenance::ConfigStore legacy_store(legacy_path);
        gateway::GatewayConfig legacy_cfg = legacy_store.load();
        assert(legacy_cfg.loaded_from_legacy);
        assert(legacy_cfg.listen.port == 8811);

        // Save Copy As to a new v2.0 file without overwriting legacy_v1.json
        auto v2_copy_path = temp_dir / "migrated_v2_copy.json";
        maintenance::ConfigStore copy_store(v2_copy_path);
        assert(copy_store.save_atomic(legacy_cfg, &err));
        assert(copy_store.exists_on_disk());

        // Verify original legacy file on disk was NOT overwritten
        {
            std::ifstream ifs(legacy_path, std::ios::binary);
            std::string disk_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            assert(disk_content == legacy_raw);
        }
        // Verify new copy is schema_version 2.0 and not marked as legacy
        {
            gateway::GatewayConfig copy_loaded = copy_store.load();
            assert(!copy_loaded.loaded_from_legacy);
            assert(copy_loaded.schema_version == "2.0");
            assert(copy_loaded.listen.port == 8811);
        }

        // 7. ProcessController active_port tracking across port transitions
        gateway::CredentialStore dummy_cred("TestLLMGateway_Phase3_H04_NotUsed", false);
        maintenance::ProcessController ctrl("nonexistent-gateway.exe", dummy_cred);
        assert(ctrl.active_port() == 0);
        ctrl.set_active_port(8765);
        assert(ctrl.active_port() == 8765);

        std::filesystem::remove_all(temp_dir, ec);
    }

    std::cout << "[PASS] Phase 3 (H-04 & H-08) regression tests.\n";
}

void test_phase4_h09_log_tail_reader() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::temp_directory_path() / "llmgw_phase4_h09_tail";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    fs::path logfile = dir / "llm-gateway-2026-10-13.log";

    auto write_file = [&](const std::string& data, bool append) {
        std::ofstream f(logfile, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    };

    {
        maintenance::LogTailReader reader(dir);
        write_file("first\r\nsecond\n", false);
        auto init = reader.read_initial_tail();
        assert(init.size() == 2 && init[0] == "first" && init[1] == "second");

        // Unterminated line is held until its newline arrives.
        write_file("thi", true);
        assert(reader.read_new_lines().empty());
        write_file("rd\n", true);
        auto l = reader.read_new_lines();
        assert(l.size() == 1 && l[0] == "third");

        // UTF-8 character (3 bytes) split across two appends is never corrupted.
        const std::string jp = "\xE6\x97\xA5\xE6\x9C\xAC"; // 日本
        write_file(jp.substr(0, 4), true);
        assert(reader.read_new_lines().empty());
        write_file(jp.substr(4) + "\n", true);
        l = reader.read_new_lines();
        assert(l.size() == 1 && l[0] == jp);

        // Truncation to a smaller file restarts from the beginning (no stale offset).
        write_file("x\n", false);
        l = reader.read_new_lines();
        assert(l.size() == 1 && l[0] == "x");
        write_file("y\n", true);
        l = reader.read_new_lines();
        assert(l.size() == 1 && l[0] == "y");

        // Large append is consumed in bounded chunks without losing lines.
        std::string big;
        const size_t line_count = 60000;
        for (size_t i = 0; i < line_count; ++i) big += "line-" + std::to_string(i) + "-padding-padding\n";
        assert(big.size() > 1048576);
        write_file(big, true);
        size_t total = 0;
        size_t first_call = reader.read_new_lines().size();
        assert(first_call > 0 && first_call < line_count);
        total += first_call;
        for (int i = 0; i < 20 && total < line_count; ++i) total += reader.read_new_lines().size();
        assert(total == line_count);
    }

    {
        // Initial tail starting mid-file drops the partial leading line.
        std::string data;
        for (int i = 0; i < 100; ++i) data += "entry-" + std::to_string(i) + "\n";
        write_file(data, false);
        maintenance::LogTailReader reader(dir);
        auto init = reader.read_initial_tail(50);
        assert(!init.empty());
        assert(init.back() == "entry-99");
        for (const auto& s : init) assert(s.rfind("entry-", 0) == 0);
    }

    fs::remove_all(dir, ec);
    std::cout << "[PASS] Phase 4 (H-09) log tail reader regression tests.\n";
}

int main() {
    test_health_endpoint();
    test_h01_sse_streaming_and_upstream_proxy();
    test_h02_request_counter_and_lifecycle();
    test_phase2_h03_h05_h06_h07_h11_h12_h14();
    test_phase3_h04_h08();
    test_phase4_h09_log_tail_reader();
    return 0;
}
