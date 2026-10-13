#pragma once

#include "gateway/config.hpp"
#include "gateway/error.hpp"

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <httplib.h>

namespace gateway {

class RequestGuard {
public:
    explicit RequestGuard(const GatewayConfig& config);

    // Validates Host header strictly. Returns GatewayError on failure.
    [[nodiscard]] std::optional<GatewayError> validate_host(const httplib::Request& req) const;

    // Checks CORS origin. Returns GatewayError on failure.
    [[nodiscard]] std::optional<GatewayError> validate_origin(const httplib::Request& req, httplib::Response& res) const;

    // Checks if request is a preflight CORS request and handles it.
    // Returns true if handled (204 or error set), false if normal request.
    bool handle_cors_preflight(const httplib::Request& req, httplib::Response& res) const;

    // Validates body size limit.
    [[nodiscard]] std::optional<GatewayError> validate_body_size(const httplib::Request& req) const;

private:
    std::string expected_host_ipv4_;
    std::string expected_host_localhost_;
    std::vector<std::string> allowed_origins_;
    size_t max_request_bytes_;
    bool cors_enabled_;
    std::vector<std::string> allowed_methods_;
    std::vector<std::string> allowed_headers_;
    uint32_t cors_max_age_sec_;
};

} // namespace gateway
