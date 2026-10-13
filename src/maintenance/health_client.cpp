#include "maintenance/health_client.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace maintenance {

std::optional<HealthResponse> HealthClient::parse_health_payload(const std::string& response) {
    if (response.empty()) {
        return std::nullopt;
    }
    try {
        auto j = nlohmann::json::parse(response);
        if (!j.is_object()) return std::nullopt;
        if (!j.contains("status") || !j["status"].is_string()) return std::nullopt;
        if (!j.contains("pid") || !j["pid"].is_number_integer()) return std::nullopt;
        if (!j.contains("version") || !j["version"].is_string()) return std::nullopt;
        if (!j.contains("embedded") || !j["embedded"].is_string()) return std::nullopt;

        HealthResponse hr;
        hr.status = j["status"].get<std::string>();
        if (hr.status != "ok") return std::nullopt;

        int64_t pid_val = j["pid"].get<int64_t>();
        if (pid_val <= 0 || pid_val > static_cast<int64_t>(UINT32_MAX)) return std::nullopt;
        hr.pid = static_cast<uint32_t>(pid_val);

        hr.version = j["version"].get<std::string>();
        if (hr.version.empty()) return std::nullopt;

        hr.embedded = j["embedded"].get<std::string>();
        if (hr.embedded != "disabled" && hr.embedded != "loading" &&
            hr.embedded != "ready" && hr.embedded != "error") {
            return std::nullopt;
        }
        return hr;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<HealthResponse> HealthClient::check_health(uint16_t port, uint32_t timeout_ms) const {
    std::string url = "http://127.0.0.1:" + std::to_string(port) + "/health";

    CURL* curl = curl_easy_init();
    if (!curl) return std::nullopt;

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* str = static_cast<std::string*>(userdata);
        size_t total = size * nmemb;
        str->append(ptr, total);
        return total;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200 || response.empty()) {
        return std::nullopt;
    }

    return parse_health_payload(response);
}

bool HealthClient::is_port_open(uint16_t port, uint32_t /*timeout_ms*/) const {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    // Non-blocking mode
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);

    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 100000; // 100ms

    int select_res = select(0, nullptr, &write_fds, nullptr, &tv);
    bool connected = false;
    if (select_res > 0) {
        int opt_err = 0;
        int opt_len = sizeof(opt_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&opt_err), &opt_len);
        connected = (opt_err == 0);
    }

    closesocket(sock);
    return connected;
}

} // namespace maintenance
