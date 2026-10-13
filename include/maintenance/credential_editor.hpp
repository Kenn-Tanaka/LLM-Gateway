#pragma once

#include "gateway/credential_store.hpp"
#include <string>
#include <vector>

namespace maintenance {

struct ProviderKeyStatus {
    std::string vendor;
    std::string slot;
    bool is_registered = false;
};

class CredentialEditor {
public:
    explicit CredentialEditor(const gateway::CredentialStore& store);

    // Queries status without retrieving the secret values
    [[nodiscard]] ProviderKeyStatus check_provider_key(const std::string& vendor, const std::string& slot) const;

    // Checks token status
    [[nodiscard]] bool check_token_exists(const std::string& token_name) const;

    // Sets key
    bool set_provider_key(const std::string& vendor, const std::string& slot, std::string_view secret);

    // Sets token
    bool set_token(const std::string& token_name, std::string_view secret);

    // Deletes key
    bool delete_provider_key(const std::string& vendor, const std::string& slot);

    // Generates a new 32-byte Base64URL token and writes to WinCred (returns empty string on failure)
    std::string generate_and_store_token(const std::string& token_name);

    // Copies to clipboard and schedules clearing clipboard after 30 seconds if unchanged
    static bool copy_to_clipboard_temporary(const std::string& text, uint32_t clear_after_sec = 30);

private:
    const gateway::CredentialStore& store_;
};

} // namespace maintenance
