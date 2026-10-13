#include "gateway/auth.hpp"

#include <cctype>

namespace gateway {

namespace {

bool starts_with_case_insensitive(std::string_view str, std::string_view prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(str[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

Authenticator::Authenticator(const GatewayConfig& config, const CredentialStore& store)
    : client_auth_required_(config.security.client_auth),
      client_credential_name_(config.security.credential_name),
      admin_credential_name_(config.security.admin_credential_name),
      store_(store) {}

std::optional<GatewayError> Authenticator::verify_bearer_token(
    const httplib::Request& req,
    const std::string& credential_name,
    const GatewayError& failure_error) const {

    size_t auth_count = req.headers.count("Authorization");
    if (auth_count != 1) {
        return failure_error;
    }

    auto auth_it = req.headers.find("Authorization");
    if (auth_it == req.headers.end()) {
        return failure_error;
    }

    const std::string& auth_val = auth_it->second;

    // Check scheme "Bearer " (ASCII case-insensitive scheme, exactly one space, no trim on token)
    if (!starts_with_case_insensitive(auth_val, "bearer ")) {
        return failure_error;
    }

    std::string_view token_val = std::string_view(auth_val).substr(7);

    // Retrieve secret from WinCred (zeroized automatically when Secret destructs)
    auto secret_opt = store_.get_token_credential(credential_name);
    if (!secret_opt) {
        return failure_error;
    }

    const Secret& secret = *secret_opt;
    if (!constant_time_equals(token_val, secret.view())) {
        return failure_error;
    }

    return std::nullopt;
}

std::optional<GatewayError> Authenticator::authenticate_client(const httplib::Request& req) const {
    if (!client_auth_required_) {
        return std::nullopt;
    }
    return verify_bearer_token(req, client_credential_name_, errors::invalid_client_token());
}

std::optional<GatewayError> Authenticator::authenticate_admin(const httplib::Request& req) const {
    return verify_bearer_token(req, admin_credential_name_, errors::invalid_admin_token());
}

} // namespace gateway
