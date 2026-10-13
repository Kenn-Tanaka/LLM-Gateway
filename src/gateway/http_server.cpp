#include "gateway/http_server.hpp"
#include "gateway/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>

#include <chrono>

namespace gateway {

namespace {

thread_local ShutdownCoordinator::RequestGuard tl_request_guard;
thread_local std::chrono::steady_clock::time_point tl_request_start;

} // namespace

HttpServer::HttpServer(
    const GatewayConfig& config,
    const CredentialStore& store,
    EmbeddedEngine& embedded_engine,
    ShutdownCoordinator& shutdown_coordinator)
    : config_(config),
      store_(store),
      embedded_engine_(embedded_engine),
      shutdown_coordinator_(shutdown_coordinator),
      request_guard_(config),
      authenticator_(config, store),
      model_router_(config),
      model_catalog_(config, store),
      external_proxy_(config, store) {

    setup_routes();
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::send_error(httplib::Response& res, const GatewayError& err) {
    res.status = err.http_status;
    res.set_content(err.to_string(), "application/json; charset=utf-8");
}

void HttpServer::setup_routes() {
    svr_.set_payload_max_length(config_.limits.max_request_bytes);
    // Configure socket for SO_EXCLUSIVEADDRUSE
    svr_.set_socket_options([](socket_t sock) {
        BOOL optval = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&optval), sizeof(optval));
    });

    // Pre-routing handler for all requests: Host, CORS, body-size, accepting check.
    // RequestGuard is held across routing AND response body / SSE streaming, and released once in set_logger.
    svr_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
        tl_request_start = std::chrono::steady_clock::now();
        tl_request_guard.release();
        tl_request_guard = shutdown_coordinator_.acquire_guard();
        if (!tl_request_guard) {
            auto err = errors::embedded_engine_disabled("Server is shutting down.");
            err.http_status = 503;
            send_error(res, err);
            return httplib::Server::HandlerResponse::Handled;
        }

        // Host validation
        auto host_err = request_guard_.validate_host(req);
        if (host_err) {
            send_error(res, *host_err);
            return httplib::Server::HandlerResponse::Handled;
        }

        // CORS preflight handling
        if (request_guard_.handle_cors_preflight(req, res)) {
            return httplib::Server::HandlerResponse::Handled;
        }

        // CORS Origin validation
        auto origin_err = request_guard_.validate_origin(req, res);
        if (origin_err) {
            send_error(res, *origin_err);
            return httplib::Server::HandlerResponse::Handled;
        }

        // Body size validation
        auto size_err = request_guard_.validate_body_size(req);
        if (size_err) {
            send_error(res, *size_err);
            return httplib::Server::HandlerResponse::Handled;
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Logger runs after response headers, body, and any SSE content_provider stream have finished.
    svr_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
        tl_request_guard.release();

        int64_t latency_ms = 0;
        if (tl_request_start.time_since_epoch().count() > 0) {
            latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tl_request_start).count();
            if (latency_ms < 0) latency_ms = 0;
            tl_request_start = {};
        }

        LogEntry entry;
        entry.level = (res.status >= 500) ? LogLevel::Error : (res.status >= 400 ? LogLevel::Warn : LogLevel::Info);
        entry.client_ip = "127.0.0.1";
        entry.method = req.method;
        entry.endpoint = req.path;
        entry.status = res.status;
        entry.latency_ms = latency_ms;
        entry.win32_error = 0;
        get_logger().log(entry);
    });

    svr_.set_exception_handler([this](const httplib::Request& /*req*/, httplib::Response& res, std::exception_ptr ep) {
        try {
            if (ep) {
                std::rethrow_exception(ep);
            }
        } catch (const GatewayException& ge) {
            send_error(res, ge.error());
            return;
        } catch (...) {
        }
        GatewayError internal_err{500, "internal_error", "Internal server error."};
        send_error(res, internal_err);
    });

    // 1. GET /health
    svr_.Get("/health", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        DWORD pid = GetCurrentProcessId();
        std::string emb_state = embedded_engine_.aggregate_state_string();

        nlohmann::json j = {
            {"status", "ok"},
            {"pid", pid},
            {"version", "2.0.0"},
            {"embedded", emb_state}
        };
        res.status = 200;
        res.set_content(j.dump(), "application/json; charset=utf-8");
    });

    // 2. POST /admin/shutdown
    svr_.Post("/admin/shutdown", [this](const httplib::Request& req, httplib::Response& res) {
        auto auth_err = authenticator_.authenticate_admin(req);
        if (auth_err) {
            send_error(res, *auth_err);
            return;
        }

        if (!req.body.empty()) {
            send_error(res, errors::bad_request("Request body must be empty."));
            return;
        }

        shutdown_coordinator_.trigger_shutdown(std::chrono::milliseconds(5000));

        nlohmann::json j = {{"status", "shutting_down"}};
        res.status = 200;
        res.set_content(j.dump(), "application/json; charset=utf-8");
    });

    // 3. GET /v1/models
    svr_.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) {
        auto auth_err = authenticator_.authenticate_client(req);
        if (auth_err) {
            send_error(res, *auth_err);
            return;
        }

        auto result = model_catalog_.get_models_list(embedded_engine_);
        if (std::holds_alternative<GatewayError>(result)) {
            send_error(res, std::get<GatewayError>(result));
            return;
        }

        res.status = 200;
        res.set_content(std::get<nlohmann::json>(result).dump(), "application/json; charset=utf-8");
    });

    // 4. POST /v1/chat/completions
    svr_.Post("/v1/chat/completions", [this](const httplib::Request& req, httplib::Response& res) {
        auto auth_err = authenticator_.authenticate_client(req);
        if (auth_err) {
            send_error(res, *auth_err);
            return;
        }

        nlohmann::json body_json;
        try {
            body_json = nlohmann::json::parse(req.body);
        } catch (...) {
            send_error(res, errors::bad_request("Malformed JSON body."));
            return;
        }

        if (!body_json.is_object() || !body_json.contains("model") || !body_json["model"].is_string()) {
            send_error(res, errors::bad_request("Missing 'model' field."));
            return;
        }

        if (body_json.contains("stream") && !body_json["stream"].is_null() && !body_json["stream"].is_boolean()) {
            send_error(res, errors::bad_request("Field 'stream' must be a boolean."));
            return;
        }

        std::string requested_model = body_json["model"].get<std::string>();
        auto route_result = model_router_.resolve(requested_model, EndpointType::ChatCompletions);
        if (std::holds_alternative<GatewayError>(route_result)) {
            send_error(res, std::get<GatewayError>(route_result));
            return;
        }

        const auto& route = std::get<ResolvedRoute>(route_result);
        const auto* provider = config_.find_provider(route.provider_id);
        if (!provider) {
            send_error(res, errors::provider_not_found());
            return;
        }

        // Replace model with upstream model
        body_json["model"] = route.upstream_model;
        std::string modified_body = body_json.dump();

        external_proxy_.forward_request(*provider, "/chat/completions", req, res, modified_body);
    });

    // 5. POST /v1/embeddings
    svr_.Post("/v1/embeddings", [this](const httplib::Request& req, httplib::Response& res) {
        auto auth_err = authenticator_.authenticate_client(req);
        if (auth_err) {
            send_error(res, *auth_err);
            return;
        }

        nlohmann::json body_json;
        try {
            body_json = nlohmann::json::parse(req.body);
        } catch (...) {
            send_error(res, errors::bad_request("Malformed JSON body."));
            return;
        }

        if (!body_json.is_object() || !body_json.contains("model") || !body_json["model"].is_string()) {
            send_error(res, errors::bad_request("Missing 'model' field."));
            return;
        }

        std::string requested_model = body_json["model"].get<std::string>();
        auto route_result = model_router_.resolve(requested_model, EndpointType::Embeddings);
        if (std::holds_alternative<GatewayError>(route_result)) {
            send_error(res, std::get<GatewayError>(route_result));
            return;
        }

        const auto& route = std::get<ResolvedRoute>(route_result);

        // Check encoding_format
        if (body_json.contains("encoding_format") && !body_json["encoding_format"].is_null()) {
            if (!body_json["encoding_format"].is_string()) {
                send_error(res, errors::bad_request("Field 'encoding_format' must be a string."));
                return;
            }
            std::string fmt = body_json["encoding_format"].get<std::string>();
            if (fmt == "base64") {
                send_error(res, errors::unsupported_encoding_format());
                return;
            } else if (fmt != "float") {
                send_error(res, errors::bad_request("Unsupported encoding_format: " + fmt));
                return;
            }
        }

        // Check input
        if (!body_json.contains("input")) {
            send_error(res, errors::bad_request("Missing 'input' field."));
            return;
        }

        std::vector<std::string> inputs;
        if (body_json["input"].is_string()) {
            std::string s = body_json["input"].get<std::string>();
            if (s.empty()) {
                send_error(res, errors::bad_request("Input string cannot be empty."));
                return;
            }
            inputs.push_back(std::move(s));
        } else if (body_json["input"].is_array()) {
            if (body_json["input"].empty()) {
                send_error(res, errors::bad_request("Input array cannot be empty."));
                return;
            }
            for (const auto& elem : body_json["input"]) {
                if (!elem.is_string()) {
                    send_error(res, errors::bad_request("Input elements must be strings."));
                    return;
                }
                std::string s = elem.get<std::string>();
                if (s.empty()) {
                    send_error(res, errors::bad_request("Input strings cannot be empty."));
                    return;
                }
                inputs.push_back(std::move(s));
            }
        } else {
            send_error(res, errors::bad_request("Input must be a string or array of strings."));
            return;
        }

        // Apply input_prefix if defined
        if (route.input_prefix) {
            for (auto& s : inputs) {
                s = *route.input_prefix + s;
            }
        }

        std::optional<int> requested_dims;
        if (body_json.contains("dimensions") && !body_json["dimensions"].is_null()) {
            if (!body_json["dimensions"].is_number_integer()) {
                send_error(res, errors::bad_request("Field 'dimensions' must be a positive integer."));
                return;
            }
            int64_t dims = body_json["dimensions"].get<int64_t>();
            if (dims < 1 || dims > 1048576) {
                send_error(res, errors::bad_request("Field 'dimensions' must be a positive integer."));
                return;
            }
            requested_dims = static_cast<int>(dims);
        }

        if (route.is_embedded) {
            // Embedded embedding calculation
            auto emb_res = embedded_engine_.embedding().compute_embeddings(inputs, requested_dims);
            if (std::holds_alternative<GatewayError>(emb_res)) {
                send_error(res, std::get<GatewayError>(emb_res));
                return;
            }

            const auto& result = std::get<EmbeddingResult>(emb_res);
            nlohmann::json data_arr = nlohmann::json::array();
            for (size_t i = 0; i < result.embeddings.size(); ++i) {
                data_arr.push_back({
                    {"object", "embedding"},
                    {"index", i},
                    {"embedding", result.embeddings[i]}
                });
            }

            nlohmann::json resp = {
                {"object", "list"},
                {"data", data_arr},
                {"model", route.canonical_model_id},
                {"usage", {
                    {"prompt_tokens", result.prompt_tokens},
                    {"total_tokens", result.total_tokens}
                }}
            };
            res.status = 200;
            res.set_content(resp.dump(), "application/json; charset=utf-8");
        } else {
            // Forward to external provider
            const auto* provider = config_.find_provider(route.provider_id);
            if (!provider) {
                send_error(res, errors::provider_not_found());
                return;
            }

            body_json["model"] = route.upstream_model;
            body_json["input"] = inputs;
            std::string modified_body = body_json.dump();

            external_proxy_.forward_request(*provider, "/embeddings", req, res, modified_body);
        }
    });

    // 6. POST /v1/rerank
    svr_.Post("/v1/rerank", [this](const httplib::Request& req, httplib::Response& res) {
        auto auth_err = authenticator_.authenticate_client(req);
        if (auth_err) {
            send_error(res, *auth_err);
            return;
        }

        nlohmann::json body_json;
        try {
            body_json = nlohmann::json::parse(req.body);
        } catch (...) {
            send_error(res, errors::bad_request("Malformed JSON body."));
            return;
        }

        if (!body_json.is_object() || !body_json.contains("model") || !body_json["model"].is_string()) {
            send_error(res, errors::bad_request("Missing 'model' field."));
            return;
        }

        std::string requested_model = body_json["model"].get<std::string>();
        auto route_result = model_router_.resolve(requested_model, EndpointType::Rerank);
        if (std::holds_alternative<GatewayError>(route_result)) {
            send_error(res, std::get<GatewayError>(route_result));
            return;
        }

        const auto& route = std::get<ResolvedRoute>(route_result);
        if (!route.is_embedded) {
            send_error(res, errors::unsupported_route());
            return;
        }

        if (!body_json.contains("query") || !body_json["query"].is_string()) {
            send_error(res, errors::bad_request("Missing or invalid 'query' field."));
            return;
        }
        std::string query = body_json["query"].get<std::string>();
        if (query.empty()) {
            send_error(res, errors::bad_request("'query' cannot be empty."));
            return;
        }

        if (!body_json.contains("documents") || !body_json["documents"].is_array() || body_json["documents"].empty()) {
            send_error(res, errors::bad_request("Missing or empty 'documents' array."));
            return;
        }

        std::vector<std::string> documents;
        for (const auto& elem : body_json["documents"]) {
            if (!elem.is_string()) {
                send_error(res, errors::bad_request("Elements in 'documents' must be strings."));
                return;
            }
            std::string doc = elem.get<std::string>();
            if (doc.empty()) {
                send_error(res, errors::bad_request("Document strings in 'documents' cannot be empty."));
                return;
            }
            documents.push_back(std::move(doc));
        }

        size_t top_n = documents.size();
        if (body_json.contains("top_n") && !body_json["top_n"].is_null()) {
            if (!body_json["top_n"].is_number_integer()) {
                send_error(res, errors::bad_request("'top_n' must be an integer between 1 and documents length."));
                return;
            }
            int64_t tn = body_json["top_n"].get<int64_t>();
            if (tn < 1 || tn > static_cast<int64_t>(documents.size())) {
                send_error(res, errors::bad_request("'top_n' must be between 1 and documents length."));
                return;
            }
            top_n = static_cast<size_t>(tn);
        }

        bool return_documents = false;
        if (body_json.contains("return_documents") && !body_json["return_documents"].is_null()) {
            if (!body_json["return_documents"].is_boolean()) {
                send_error(res, errors::bad_request("Field 'return_documents' must be a boolean."));
                return;
            }
            return_documents = body_json["return_documents"].get<bool>();
        }

        auto rerank_res = embedded_engine_.reranker().compute_rerank(query, documents, top_n, return_documents);
        if (std::holds_alternative<GatewayError>(rerank_res)) {
            send_error(res, std::get<GatewayError>(rerank_res));
            return;
        }

        const auto& result = std::get<RerankResult>(rerank_res);
        nlohmann::json results_arr = nlohmann::json::array();
        for (const auto& item : result.results) {
            nlohmann::json r = {
                {"index", item.index},
                {"relevance_score", item.relevance_score}
            };
            if (return_documents) {
                r["document"] = {{"text", item.document_text}};
            }
            results_arr.push_back(r);
        }

        nlohmann::json resp = {
            {"id", result.id},
            {"results", results_arr}
        };

        res.status = 200;
        res.set_content(resp.dump(), "application/json; charset=utf-8");
    });

    // 7. Undefined endpoints -> 404 endpoint_not_found per spec 12.3
    svr_.set_error_handler([this](const httplib::Request& /*req*/, httplib::Response& res) {
        if (res.status == 413) {
            send_error(res, errors::request_body_too_large());
        }
        if (res.status == 404 && res.body.empty()) {
            send_error(res, errors::endpoint_not_found());
        }
    });
}

bool HttpServer::start() {
    get_logger().info("HttpServer: binding to " + config_.listen.host + ":" + std::to_string(config_.listen.port));
    return svr_.listen(config_.listen.host, config_.listen.port);
}

void HttpServer::wait_until_ready() const {
    svr_.wait_until_ready();
}

void HttpServer::stop() {
    svr_.stop();
}

} // namespace gateway
