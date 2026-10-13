#include "gateway/request_guard.hpp"
#include <cassert>
#include <iostream>

void test_request_guard() {
    gateway::GatewayConfig config;
    config.listen.port = 8765;
    config.security.allowed_origins = {"http://localhost:5173"};
    config.limits.max_request_bytes = 1000;

    gateway::RequestGuard guard(config);

    // 1. Host header validation
    httplib::Request req_valid_ipv4;
    req_valid_ipv4.headers.emplace("Host", "127.0.0.1:8765");
    assert(!guard.validate_host(req_valid_ipv4).has_value());

    httplib::Request req_valid_localhost;
    req_valid_localhost.headers.emplace("Host", "localhost:8765");
    assert(!guard.validate_host(req_valid_localhost).has_value());

    httplib::Request req_bad_host;
    req_bad_host.headers.emplace("Host", "attacker.com:8765");
    auto host_err = guard.validate_host(req_bad_host);
    assert(host_err.has_value());
    assert(host_err->code == "host_mismatch");

    httplib::Request req_no_host;
    assert(guard.validate_host(req_no_host).has_value());

    // 2. CORS Origin validation
    httplib::Request req_native; // No Origin
    httplib::Response res_native;
    assert(!guard.validate_origin(req_native, res_native).has_value());

    httplib::Request req_allowed_origin;
    req_allowed_origin.headers.emplace("Origin", "http://localhost:5173");
    httplib::Response res_allowed;
    assert(!guard.validate_origin(req_allowed_origin, res_allowed).has_value());
    assert(res_allowed.get_header_value("Access-Control-Allow-Origin") == "http://localhost:5173");

    httplib::Request req_denied_origin;
    req_denied_origin.headers.emplace("Origin", "http://malicious-site.com");
    httplib::Response res_denied;
    auto cors_err = guard.validate_origin(req_denied_origin, res_denied);
    assert(cors_err.has_value());
    assert(cors_err->code == "cors_origin_denied");

    // 3. Body size validation
    httplib::Request req_small_body;
    req_small_body.body = std::string(500, 'x');
    assert(!guard.validate_body_size(req_small_body).has_value());

    httplib::Request req_large_body;
    req_large_body.body = std::string(1500, 'x');
    auto size_err = guard.validate_body_size(req_large_body);
    assert(size_err.has_value());
    assert(size_err->code == "request_body_too_large");

    std::cout << "[PASS] RequestGuard unit tests.\n";
}

int main() {
    test_request_guard();
    return 0;
}
