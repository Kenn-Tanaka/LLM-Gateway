#pragma once

#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/error.hpp"

#include <string>
#include <httplib.h>

namespace gateway {

class ExternalProxy {
public:
    ExternalProxy(const GatewayConfig& config, const CredentialStore& store);
    ~ExternalProxy();

    // Proxies a request to upstream provider and handles both non-stream and SSE stream responses.
    void forward_request(
        const ProviderConfig& provider,
        const std::string& upstream_subpath,
        const httplib::Request& client_req,
        httplib::Response& client_res,
        const std::string& modified_body);

private:
    const GatewayConfig& config_;
    const CredentialStore& store_;
};

} // namespace gateway
