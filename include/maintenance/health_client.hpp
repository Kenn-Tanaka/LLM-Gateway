#pragma once

#include <string>
#include <optional>
#include <cstdint>

namespace maintenance {

struct HealthResponse {
    std::string status;
    uint32_t pid = 0;
    std::string version;
    std::string embedded;
};

class HealthClient {
public:
    HealthClient() = default;

    // Queries GET http://127.0.0.1:<port>/health
    [[nodiscard]] std::optional<HealthResponse> check_health(uint16_t port, uint32_t timeout_ms = 500) const;

    // Parses and validates a /health JSON payload
    [[nodiscard]] static std::optional<HealthResponse> parse_health_payload(const std::string& response);

    // Checks if port is responding or refusing connection
    [[nodiscard]] bool is_port_open(uint16_t port, uint32_t timeout_ms = 100) const;
};

} // namespace maintenance
