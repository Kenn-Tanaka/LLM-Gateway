#pragma once

#include <string>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace gateway {

struct GatewayError {
    int http_status;
    std::string code;
    std::string message;

    [[nodiscard]] nlohmann::json to_json() const {
        return nlohmann::json{
            {"error", {
                {"message", message},
                {"type", "gateway_error"},
                {"code", code}
            }}
        };
    }

    [[nodiscard]] std::string to_string() const {
        return to_json().dump();
    }
};

class GatewayException : public std::exception {
public:
    GatewayException(int http_status, std::string code, std::string message)
        : error_{http_status, std::move(code), std::move(message)} {}

    [[nodiscard]] const char* what() const noexcept override {
        return error_.message.c_str();
    }

    [[nodiscard]] const GatewayError& error() const noexcept {
        return error_;
    }

private:
    GatewayError error_;
};

namespace errors {

inline GatewayError invalid_client_token(std::string msg = "Client authentication failed.") {
    return {401, "invalid_client_token", std::move(msg)};
}

inline GatewayError invalid_admin_token(std::string msg = "Admin authentication failed.") {
    return {401, "invalid_admin_token", std::move(msg)};
}

inline GatewayError host_mismatch(std::string msg = "Host header mismatch.") {
    return {403, "host_mismatch", std::move(msg)};
}

inline GatewayError cors_origin_denied(std::string msg = "CORS origin denied.") {
    return {403, "cors_origin_denied", std::move(msg)};
}

inline GatewayError gateway_model_forbidden(std::string msg = "Model access denied by gateway allowlist.") {
    return {403, "gateway_model_forbidden", std::move(msg)};
}

inline GatewayError endpoint_not_found(std::string msg = "Endpoint not found.") {
    return {404, "endpoint_not_found", std::move(msg)};
}

inline GatewayError provider_not_found(std::string msg = "Provider not found.") {
    return {404, "provider_not_found", std::move(msg)};
}

inline GatewayError request_body_too_large(std::string msg = "Request body exceeds configured limit.") {
    return {413, "request_body_too_large", std::move(msg)};
}

inline GatewayError unsupported_encoding_format(std::string msg = "Encoding format 'base64' is not supported.") {
    return {400, "unsupported_encoding_format", std::move(msg)};
}

inline GatewayError unsupported_dimensions(std::string msg = "Requested dimensions not supported for this model.") {
    return {400, "unsupported_dimensions", std::move(msg)};
}

inline GatewayError unsupported_route(std::string msg = "Unsupported route for requested endpoint.") {
    return {400, "unsupported_route", std::move(msg)};
}

inline GatewayError bad_request(std::string msg = "Bad request.") {
    return {400, "bad_request", std::move(msg)};
}

inline GatewayError upstream_connection_failed(std::string msg = "Connection to upstream provider failed.") {
    return {502, "upstream_connection_failed", std::move(msg)};
}

inline GatewayError upstream_timeout(std::string msg = "Upstream provider read timed out.") {
    return {504, "upstream_timeout", std::move(msg)};
}

inline GatewayError embedded_engine_disabled(std::string msg = "Embedded engine is disabled.") {
    return {503, "embedded_engine_disabled", std::move(msg)};
}

inline GatewayError embedded_loading(std::string msg = "Embedded model is still loading.") {
    return {503, "embedded_loading", std::move(msg)};
}

inline GatewayError embedded_load_failed(std::string msg = "Embedded model failed to load.") {
    return {503, "embedded_load_failed", std::move(msg)};
}

} // namespace errors

} // namespace gateway
