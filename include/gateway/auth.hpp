#pragma once

#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/error.hpp"

#include <optional>
#include <string_view>
#include <httplib.h>

namespace gateway {

class Authenticator {
public:
    Authenticator(const GatewayConfig& config, const CredentialStore& store);

    // Validates Client Bearer Token for normal endpoints.
    [[nodiscard]] std::optional<GatewayError> authenticate_client(const httplib::Request& req) const;

    // Validates Admin Bearer Token for /admin/shutdown.
    [[nodiscard]] std::optional<GatewayError> authenticate_admin(const httplib::Request& req) const;

private:
    bool client_auth_required_;
    std::string client_credential_name_;
    std::string admin_credential_name_;
    const CredentialStore& store_;

    [[nodiscard]] std::optional<GatewayError> verify_bearer_token(
        const httplib::Request& req,
        const std::string& credential_name,
        const GatewayError& failure_error) const;
};

} // namespace gateway
