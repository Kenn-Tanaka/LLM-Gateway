#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/embedded_engine.hpp"
#include "gateway/shutdown_coordinator.hpp"
#include "gateway/http_server.hpp"
#include "gateway/logger.hpp"

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>
#include <curl/curl.h>

void test_security_protections() {
    gateway::GatewayConfig config;
    config.listen.host = "127.0.0.1";
    config.listen.port = 18766;
    config.embedded_engine.enabled = false;
    config.security.allowed_origins = {"http://trusted.local:5173"};

    gateway::CredentialStore store("CloudLLM", false);
    gateway::EmbeddedEngine embedded_engine(config, true);
    gateway::ShutdownCoordinator shutdown_coordinator;

    gateway::HttpServer server(config, store, embedded_engine, shutdown_coordinator);

    std::thread server_thread([&server]() {
        server.start();
    });

    server.wait_until_ready();

    // 1. DNS Rebinding simulation: connecting to 127.0.0.1:18766 with Host: evil.com
    CURL* curl = curl_easy_init();
    assert(curl);

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Host: evil.com:18766");
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:18766/health");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    long http_code = 0;
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    assert(http_code == 403); // host_mismatch
    assert(shutdown_coordinator.active_requests() == 0);

    // 2. Disallowed Origin simulation
    curl_slist_free_all(headers);
    headers = nullptr;
    headers = curl_slist_append(headers, "Host: 127.0.0.1:18766");
    headers = curl_slist_append(headers, "Origin: http://evil.com");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    http_code = 0;
    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    assert(http_code == 403); // cors_origin_denied
    assert(shutdown_coordinator.active_requests() == 0);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }

    std::cout << "[PASS] Security protections tests (DNS rebinding & CORS enforcement).\n";
}

int main() {
    test_security_protections();
    return 0;
}
