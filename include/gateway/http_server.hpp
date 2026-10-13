#pragma once

#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/request_guard.hpp"
#include "gateway/auth.hpp"
#include "gateway/model_router.hpp"
#include "gateway/model_catalog.hpp"
#include "gateway/external_proxy.hpp"
#include "gateway/embedded_engine.hpp"
#include "gateway/shutdown_coordinator.hpp"

#include <memory>
#include <httplib.h>

namespace gateway {

class HttpServer {
public:
    HttpServer(
        const GatewayConfig& config,
        const CredentialStore& store,
        EmbeddedEngine& embedded_engine,
        ShutdownCoordinator& shutdown_coordinator);

    ~HttpServer();

    // Starts listening synchronously.
    bool start();

    // Waits until the server socket is ready to accept connections (or decommissioned).
    void wait_until_ready() const;

    // Stops server.
    void stop();

private:
    const GatewayConfig& config_;
    const CredentialStore& store_;
    EmbeddedEngine& embedded_engine_;
    ShutdownCoordinator& shutdown_coordinator_;

    RequestGuard request_guard_;
    Authenticator authenticator_;
    ModelRouter model_router_;
    ModelCatalog model_catalog_;
    ExternalProxy external_proxy_;

    httplib::Server svr_;

    void setup_routes();
    void send_error(httplib::Response& res, const GatewayError& err);
};

} // namespace gateway
