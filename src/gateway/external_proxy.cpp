#include "gateway/external_proxy.hpp"
#include "gateway/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <curl/curl.h>

#include <algorithm>
#include <vector>
#include <atomic>
#include <chrono>

namespace gateway {

namespace {

struct UploadDeadline {
    std::chrono::steady_clock::time_point last_progress = std::chrono::steady_clock::now();
    curl_off_t uploaded = 0;
    uint32_t timeout_ms;
    bool timed_out = false;
};

int check_upload_progress(void* userdata, curl_off_t, curl_off_t, curl_off_t total, curl_off_t uploaded) {
    auto& deadline = *static_cast<UploadDeadline*>(userdata);
    const auto now = std::chrono::steady_clock::now();
    if (uploaded != deadline.uploaded || total == 0) {
        deadline.uploaded = uploaded;
        deadline.last_progress = now;
    }
    if (total > uploaded && std::chrono::duration_cast<std::chrono::milliseconds>(now - deadline.last_progress).count() >= deadline.timeout_ms) {
        deadline.timed_out = true;
        return 1;
    }
    return 0;
}

void set_upload_deadline(CURL* curl, UploadDeadline& deadline) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, check_upload_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &deadline);
}

std::atomic<int> s_curl_init_count{0};

void ensure_curl_initialized() {
    if (s_curl_init_count.fetch_add(1) == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
}

void cleanup_curl_if_needed() {
    if (s_curl_init_count.fetch_sub(1) == 1) {
        curl_global_cleanup();
    }
}

std::string to_lower_ascii(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

struct HeaderData {
    int status_code = 200;
    bool saw_status_line = false;
    bool headers_complete = false;
    std::string content_type = "application/json";
    bool is_sse = false;
    std::vector<std::pair<std::string, std::string>> safe_headers;
};

size_t header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    auto* hd = static_cast<HeaderData*>(userdata);

    std::string line(buffer, total);
    // Strip trailing CRLF
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }

    if (line.empty()) {
        if (hd->saw_status_line && hd->status_code >= 200) {
            hd->headers_complete = true;
        }
        return total;
    }

    if (line.rfind("HTTP/", 0) == 0) {
        // Status line (reset if following a 1xx informational response)
        size_t sp1 = line.find(' ');
        if (sp1 != std::string::npos) {
            size_t sp2 = line.find(' ', sp1 + 1);
            std::string code_str = (sp2 != std::string::npos)
                ? line.substr(sp1 + 1, sp2 - sp1 - 1)
                : line.substr(sp1 + 1);
            try {
                int code = std::stoi(code_str);
                hd->status_code = code;
                hd->saw_status_line = true;
                if (code >= 200) {
                    hd->safe_headers.clear();
                    hd->content_type = "application/json";
                    hd->is_sse = false;
                    hd->headers_complete = false;
                }
            } catch (...) {}
        }
        return total;
    }

    if (hd->headers_complete) {
        // Ignore chunked trailers after response headers are finalized
        return total;
    }

    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        // Trim key & val
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.front()))) key.erase(key.begin());
        while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
        while (!val.empty() && std::isspace(static_cast<unsigned char>(val.back()))) val.pop_back();

        std::string lower_key = to_lower_ascii(key);

        if (lower_key == "content-type") {
            hd->content_type = val;
            if (to_lower_ascii(val).find("text/event-stream") != std::string::npos) {
                hd->is_sse = true;
            }
            return total;
        }

        // Sanitize: do not forward cookies, hop-by-hop headers, framing headers, or auth headers
        static const std::vector<std::string> forbidden_headers = {
            "set-cookie", "cookie", "authorization", "proxy-authorization",
            "connection", "keep-alive", "transfer-encoding", "te", "trailer", "upgrade",
            "content-length"
        };
        if (std::find(forbidden_headers.begin(), forbidden_headers.end(), lower_key) != forbidden_headers.end()) {
            return total;
        }

        hd->safe_headers.emplace_back(std::move(key), std::move(val));
    }

    return total;
}

bool is_top_level_stream_request(const std::string& body) {
    if (body.empty()) {
        return false;
    }
    try {
        auto j = nlohmann::json::parse(body);
        if (j.is_object() && j.contains("stream") && j["stream"].is_boolean()) {
            return j["stream"].get<bool>();
        }
    } catch (...) {
    }
    return false;
}

struct UpstreamCurlTransfer {
    CURLM* multi = nullptr;
    CURL* easy = nullptr;
    curl_slist* req_headers = nullptr;
    std::string auth_header_buf;
    std::string request_body;
    HeaderData header_data;
    UploadDeadline deadline;
    std::string buffered_body;
    httplib::DataSink* active_sink = nullptr;
    bool sink_write_failed = false;
    bool transfer_finished = false;
    CURLcode curl_result = CURLE_OK;

    explicit UpstreamCurlTransfer(uint32_t write_timeout_ms)
        : deadline{std::chrono::steady_clock::now(), 0, write_timeout_ms, false} {}

    ~UpstreamCurlTransfer() {
        cleanup();
    }

    UpstreamCurlTransfer(const UpstreamCurlTransfer&) = delete;
    UpstreamCurlTransfer& operator=(const UpstreamCurlTransfer&) = delete;

    static size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        size_t total = size * nmemb;
        auto* self = static_cast<UpstreamCurlTransfer*>(userdata);
        self->header_data.headers_complete = true;
        if (total == 0) {
            return 0;
        }
        if (self->active_sink) {
            if (!self->active_sink->is_writable()) {
                self->sink_write_failed = true;
                return 0;
            }
            if (!self->active_sink->write(ptr, total)) {
                self->sink_write_failed = true;
                return 0;
            }
            return total;
        }
        self->buffered_body.append(ptr, total);
        return total;
    }

    bool init(
        const GatewayConfig& config,
        const CredentialStore& store,
        const ProviderConfig& provider,
        const std::string& target_url,
        const httplib::Request& client_req,
        const std::string& body,
        bool force_post) {

        easy = curl_easy_init();
        if (!easy) {
            return false;
        }
        multi = curl_multi_init();
        if (!multi) {
            curl_easy_cleanup(easy);
            easy = nullptr;
            return false;
        }

        request_body = body;

        // Forward safe client headers
        for (const auto& [key, val] : client_req.headers) {
            std::string lower = to_lower_ascii(key);
            if (lower == "content-type" || lower == "accept" || lower == "user-agent") {
                std::string h = key + ": " + val;
                req_headers = curl_slist_append(req_headers, h.c_str());
            }
        }
        if (!client_req.has_header("Content-Type")) {
            req_headers = curl_slist_append(req_headers, "Content-Type: application/json");
        }

        // Inject Provider Authorization if bearer
        if (provider.authentication == Authentication::Bearer) {
            auto secret_opt = store.get_provider_credential(provider.key_vendor, provider.key_slot);
            if (secret_opt) {
                auth_header_buf = "Authorization: Bearer ";
                auth_header_buf.append(secret_opt->view());
                req_headers = curl_slist_append(req_headers, auth_header_buf.c_str());
            }
        }

        curl_easy_setopt(easy, CURLOPT_URL, target_url.c_str());
        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, req_headers);

        if (force_post || client_req.method == "POST") {
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request_body.c_str());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
        }

        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(config.timeouts.upstream_connect_ms));
        curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeouts.upstream_read_ms));
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);

        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &header_data);

        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &UpstreamCurlTransfer::write_callback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);

        deadline.last_progress = std::chrono::steady_clock::now();
        set_upload_deadline(easy, deadline);

        if (curl_multi_add_handle(multi, easy) != CURLM_OK) {
            cleanup();
            return false;
        }
        return true;
    }

    template <typename StopPred, typename CancelPred>
    void step_until(StopPred stop_pred, CancelPred cancel_pred) {
        while (!transfer_finished && !stop_pred()) {
            if (cancel_pred()) {
                sink_write_failed = true;
                break;
            }

            int still_running = 0;
            CURLMcode mc = curl_multi_perform(multi, &still_running);
            if (mc != CURLM_OK) {
                curl_result = CURLE_SEND_ERROR;
                transfer_finished = true;
                break;
            }

            int msgs_in_queue = 0;
            while (CURLMsg* msg = curl_multi_info_read(multi, &msgs_in_queue)) {
                if (msg->msg == CURLMSG_DONE) {
                    curl_result = msg->data.result;
                    transfer_finished = true;
                }
            }

            if (transfer_finished || still_running == 0) {
                transfer_finished = true;
                break;
            }

            if (stop_pred()) {
                break;
            }

            if (cancel_pred()) {
                sink_write_failed = true;
                break;
            }

            int numfds = 0;
            curl_multi_poll(multi, nullptr, 0, 20, &numfds);
        }
    }

    void cleanup() {
        active_sink = nullptr;
        if (multi && easy) {
            curl_multi_remove_handle(multi, easy);
        }
        if (easy) {
            curl_easy_cleanup(easy);
            easy = nullptr;
        }
        if (multi) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
        if (req_headers) {
            curl_slist_free_all(req_headers);
            req_headers = nullptr;
        }
        if (!auth_header_buf.empty()) {
            SecureZeroMemory(auth_header_buf.data(), auth_header_buf.size());
            auth_header_buf.clear();
        }
    }
};

void set_upstream_curl_error(httplib::Response& client_res, CURLcode curl_res, const UploadDeadline& deadline) {
    if (curl_res == CURLE_OPERATION_TIMEDOUT || deadline.timed_out) {
        auto err = errors::upstream_timeout();
        client_res.status = err.http_status;
        client_res.set_content(err.to_string(), "application/json; charset=utf-8");
    } else {
        auto err = errors::upstream_connection_failed();
        client_res.status = err.http_status;
        client_res.set_content(err.to_string(), "application/json; charset=utf-8");
    }
}

} // namespace

ExternalProxy::ExternalProxy(const GatewayConfig& config, const CredentialStore& store)
    : config_(config), store_(store) {
    ensure_curl_initialized();
}

ExternalProxy::~ExternalProxy() {
    cleanup_curl_if_needed();
}

void ExternalProxy::forward_request(
    const ProviderConfig& provider,
    const std::string& upstream_subpath,
    const httplib::Request& client_req,
    httplib::Response& client_res,
    const std::string& modified_body) {

    if (!provider.base_url) {
        auto err = errors::provider_not_found();
        client_res.status = err.http_status;
        client_res.set_content(err.to_string(), "application/json; charset=utf-8");
        return;
    }

    std::string target_url = *provider.base_url;
    if (!target_url.empty() && target_url.back() == '/' && !upstream_subpath.empty() && upstream_subpath.front() == '/') {
        target_url.pop_back();
    }
    target_url += upstream_subpath;

    // Check top-level boolean "stream": true in JSON body
    const bool client_requested_stream = is_top_level_stream_request(modified_body);

    auto transfer = std::make_shared<UpstreamCurlTransfer>(config_.timeouts.upstream_write_ms);
    if (!transfer->init(config_, store_, provider, target_url, client_req, modified_body, client_requested_stream)) {
        auto err = errors::upstream_connection_failed();
        client_res.status = err.http_status;
        client_res.set_content(err.to_string(), "application/json; charset=utf-8");
        return;
    }

    if (client_requested_stream) {
        // Drive the single upstream transfer until HTTP response headers are finalized (or transfer ends/fails)
        transfer->step_until(
            [&]() { return transfer->header_data.headers_complete || transfer->transfer_finished; },
            []() { return false; }
        );

        if (transfer->curl_result != CURLE_OK) {
            CURLcode code = transfer->curl_result;
            UploadDeadline dl = transfer->deadline;
            transfer->cleanup();
            set_upstream_curl_error(client_res, code, dl);
            return;
        }

        // If upstream returned an error status (e.g. 400/401/429/500) or non-SSE content type,
        // drain the response body and return the exact upstream status and body to the client.
        if (transfer->header_data.status_code >= 400 || !transfer->header_data.is_sse) {
            if (!transfer->transfer_finished) {
                transfer->step_until(
                    [&]() { return transfer->transfer_finished; },
                    []() { return false; }
                );
            }

            CURLcode code = transfer->curl_result;
            UploadDeadline dl = transfer->deadline;
            transfer->cleanup();

            if (code != CURLE_OK) {
                set_upstream_curl_error(client_res, code, dl);
                return;
            }

            client_res.status = transfer->header_data.status_code;
            for (const auto& [hk, hv] : transfer->header_data.safe_headers) {
                client_res.set_header(hk, hv);
            }
            client_res.set_content(std::move(transfer->buffered_body), transfer->header_data.content_type);
            return;
        }

        // Upstream confirmed 2xx SSE response: forward status & headers, then stream body chunks once
        client_res.status = transfer->header_data.status_code;
        for (const auto& [hk, hv] : transfer->header_data.safe_headers) {
            client_res.set_header(hk, hv);
        }

        client_res.set_chunked_content_provider(
            transfer->header_data.content_type,
            [transfer](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                if (!transfer->buffered_body.empty()) {
                    if (!sink.is_writable() || !sink.write(transfer->buffered_body.data(), transfer->buffered_body.size())) {
                        transfer->buffered_body.clear();
                        transfer->cleanup();
                        return false;
                    }
                    transfer->buffered_body.clear();
                }

                if (!transfer->transfer_finished) {
                    transfer->active_sink = &sink;
                    transfer->step_until(
                        [&]() { return transfer->transfer_finished; },
                        [&]() { return !sink.is_writable(); }
                    );
                    transfer->active_sink = nullptr;
                }

                CURLcode final_res = transfer->curl_result;
                bool sink_failed = transfer->sink_write_failed;
                transfer->cleanup();

                if (sink_failed || final_res != CURLE_OK) {
                    if (!sink_failed && final_res != CURLE_OK) {
                        get_logger().error("Upstream SSE stream interrupted (CURLcode=" + std::to_string(static_cast<int>(final_res)) + ").");
                    }
                    return false;
                }

                sink.done();
                return true;
            },
            [transfer](bool /*success*/) {
                transfer->cleanup();
            }
        );
        return;
    }

    // Non-streaming mode: complete transfer and return buffered response
    transfer->step_until(
        [&]() { return transfer->transfer_finished; },
        []() { return false; }
    );

    CURLcode curl_res = transfer->curl_result;
    UploadDeadline dl = transfer->deadline;
    transfer->cleanup();

    if (curl_res != CURLE_OK) {
        set_upstream_curl_error(client_res, curl_res, dl);
        return;
    }

    client_res.status = transfer->header_data.status_code;
    for (const auto& [hk, hv] : transfer->header_data.safe_headers) {
        client_res.set_header(hk, hv);
    }
    client_res.set_content(std::move(transfer->buffered_body), transfer->header_data.content_type);
}

} // namespace gateway
