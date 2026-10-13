#pragma once

#include "gateway/secret.hpp"
#include <string>
#include <optional>
#include <vector>

namespace gateway {

class CredentialStore {
public:
    explicit CredentialStore(std::string service_name = "CloudLLM", bool environment_fallback = false);

    // Formats target name: "<service_name>/<vendor>|<slot>"
    [[nodiscard]] std::string format_target_name(const std::string& vendor, const std::string& slot) const;

    // Formats environment variable name: "LLM_GATEWAY_KEY_<VENDOR>_<SLOT>"
    [[nodiscard]] static std::string format_env_var_name(const std::string& vendor, const std::string& slot);

    // Reads secret for provider (supports environment variable fallback if enabled)
    [[nodiscard]] std::optional<Secret> get_provider_credential(const std::string& vendor, const std::string& slot) const;

    // Reads secret for token (client token or admin token, no env fallback allowed)
    [[nodiscard]] std::optional<Secret> get_token_credential(const std::string& target_name_or_slot) const;

    // Checks if credential exists in WinCred without leaking value
    [[nodiscard]] bool has_credential(const std::string& target_name) const;

    // Writes credential into WinCred (without trailing NUL, CRED_PERSIST_LOCAL_MACHINE)
    bool set_credential(const std::string& target_name, std::string_view secret_utf8) const;

    // Deletes credential from WinCred
    bool delete_credential(const std::string& target_name) const;

    // Generates a cryptographically secure 32-byte Base64URL token (43 chars, no padding)
    [[nodiscard]] static std::string generate_secure_token();

    // Lists existing targets with service prefix
    [[nodiscard]] std::vector<std::string> list_service_credentials() const;

private:
    std::string service_name_;
    bool environment_fallback_;

    [[nodiscard]] std::optional<Secret> read_wincred(const std::string& target_name) const;
};

} // namespace gateway
