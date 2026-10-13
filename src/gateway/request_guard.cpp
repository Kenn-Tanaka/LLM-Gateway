#include "gateway/request_guard.hpp"

#include <algorithm>
#include <sstream>

namespace gateway {

namespace {

std::string join_headers(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) {
        if (!result.empty()) result += ", ";
        result += value;
    }
    return result;
}

std::string to_lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::vector<std::string> split_comma_trimmed(std::string_view s) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < s.size()) {
        size_t comma = s.find(',', start);
        if (comma == std::string_view::npos) comma = s.size();

        // Trim
        size_t sub_start = start;
        while (sub_start < comma && std::isspace(static_cast<unsigned char>(s[sub_start]))) {
            ++sub_start;
        }
        size_t sub_end = comma;
        while (sub_end > sub_start && std::isspace(static_cast<unsigned char>(s[sub_end - 1]))) {
            --sub_end;
        }

        if (sub_end > sub_start) {
            parts.emplace_back(s.substr(sub_start, sub_end - sub_start));
        }

        start = comma + 1;
    }
    return parts;
}

} // namespace

RequestGuard::RequestGuard(const GatewayConfig& config)
    : expected_host_ipv4_("127.0.0.1:" + std::to_string(config.listen.port)),
      expected_host_localhost_("localhost:" + std::to_string(config.listen.port)),
      allowed_origins_(config.security.allowed_origins),
      max_request_bytes_(config.limits.max_request_bytes),
      cors_enabled_(config.security.cors_enabled),
      allowed_methods_(config.security.allowed_methods),
      allowed_headers_(config.security.allowed_headers),
      cors_max_age_sec_(config.security.cors_max_age_sec) {}

std::optional<GatewayError> RequestGuard::validate_host(const httplib::Request& req) const {
    size_t host_count = req.headers.count("Host");
    if (host_count != 1) {
        return errors::host_mismatch();
    }

    auto host_it = req.headers.find("Host");
    if (host_it == req.headers.end()) {
        return errors::host_mismatch();
    }

    const std::string& host_val = host_it->second;
    if (host_val != expected_host_ipv4_ && host_val != expected_host_localhost_) {
        return errors::host_mismatch();
    }

    return std::nullopt;
}

std::optional<GatewayError> RequestGuard::validate_origin(const httplib::Request& req, httplib::Response& res) const {
    if (!req.has_header("Origin")) {
        // Native client without Origin is allowed
        return std::nullopt;
    }

    std::string origin = req.get_header_value("Origin");
    auto it = std::find(allowed_origins_.begin(), allowed_origins_.end(), origin);
    if (!cors_enabled_ || it == allowed_origins_.end()) {
        return errors::cors_origin_denied();
    }

    // Origin matched
    res.set_header("Access-Control-Allow-Origin", origin);
    res.set_header("Vary", "Origin");
    return std::nullopt;
}

bool RequestGuard::handle_cors_preflight(const httplib::Request& req, httplib::Response& res) const {
    if (req.method != "OPTIONS") {
        return false;
    }

    if (!req.has_header("Origin") || !req.has_header("Access-Control-Request-Method")) {
        // Non-preflight OPTIONS -> 404 endpoint_not_found per spec 8.3
        auto err = errors::endpoint_not_found();
        res.status = err.http_status;
        res.set_content(err.to_string(), "application/json; charset=utf-8");
        return true;
    }

    // Preflight request
    std::string origin = req.get_header_value("Origin");
    auto it = std::find(allowed_origins_.begin(), allowed_origins_.end(), origin);
    if (!cors_enabled_ || it == allowed_origins_.end()) {
        auto err = errors::cors_origin_denied();
        res.status = err.http_status;
        res.set_content(err.to_string(), "application/json; charset=utf-8");
        return true;
    }

    std::string method = req.get_header_value("Access-Control-Request-Method");
    if (std::find(allowed_methods_.begin(), allowed_methods_.end(), method) == allowed_methods_.end()) {
        auto err = errors::cors_origin_denied();
        res.status = err.http_status;
        res.set_content(err.to_string(), "application/json; charset=utf-8");
        return true;
    }

    if (req.has_header("Access-Control-Request-Headers")) {
        std::string req_headers = req.get_header_value("Access-Control-Request-Headers");
        auto header_list = split_comma_trimmed(req_headers);
        for (const auto& h : header_list) {
            std::string lower = to_lower_ascii(h);
            if (std::none_of(allowed_headers_.begin(), allowed_headers_.end(), [&](const auto& allowed) { return to_lower_ascii(allowed) == lower; })) {
                auto err = errors::cors_origin_denied();
                res.status = err.http_status;
                res.set_content(err.to_string(), "application/json; charset=utf-8");
                return true;
            }
        }
    }

    // Preflight successful: 204 No Content
    res.status = 204;
    res.set_header("Access-Control-Allow-Origin", origin);
    res.set_header("Vary", "Origin");
    res.set_header("Access-Control-Allow-Methods", join_headers(allowed_methods_));
    res.set_header("Access-Control-Allow-Headers", join_headers(allowed_headers_));
    res.set_header("Access-Control-Max-Age", std::to_string(cors_max_age_sec_));
    return true;
}

std::optional<GatewayError> RequestGuard::validate_body_size(const httplib::Request& req) const {
    if (req.body.size() > max_request_bytes_) {
        return errors::request_body_too_large();
    }
    return std::nullopt;
}

} // namespace gateway
