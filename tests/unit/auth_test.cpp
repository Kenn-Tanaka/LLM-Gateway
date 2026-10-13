#include "gateway/secret.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/auth.hpp"
#include <cassert>
#include <iostream>

namespace {

struct ScopedSyntheticCredential {
    gateway::CredentialStore& store;
    std::string name;

    ScopedSyntheticCredential(gateway::CredentialStore& s, std::string n, std::string_view secret)
        : store(s), name(std::move(n)) {
        bool ok = store.set_credential(name, secret);
        assert(ok);
    }

    ~ScopedSyntheticCredential() {
        store.delete_credential(name);
    }
};

void test_authenticator_unit() {
    // Use an isolated synthetic service name so real CloudLLM/* credentials are never touched.
    gateway::CredentialStore store("LLMGatewayPhase1AuthTest_Synthetic", false);

    gateway::GatewayConfig cfg;
    cfg.credentials.service_name = "LLMGatewayPhase1AuthTest_Synthetic";
    cfg.security.client_auth = false;
    cfg.security.credential_name = "client_token_slot";
    cfg.security.admin_credential_name = "admin_token_slot";

    // 1. When client_auth is false, authenticate_client always succeeds even without Authorization header
    {
        gateway::Authenticator auth(cfg, store);
        httplib::Request req;
        assert(!auth.authenticate_client(req).has_value());
    }

    // 2. When client_auth is true and credential is not registered in WinCred -> 401 invalid_client_token
    cfg.security.client_auth = true;
    store.delete_credential(cfg.security.credential_name);
    store.delete_credential(cfg.security.admin_credential_name);
    {
        gateway::Authenticator auth(cfg, store);
        httplib::Request req;
        req.headers.emplace("Authorization", "Bearer some_token");
        auto err = auth.authenticate_client(req);
        assert(err.has_value());
        assert(err->http_status == 401);
        assert(err->code == "invalid_client_token");
    }

    // 3. Register synthetic client and admin tokens and verify header parsing & constant-time matching
    ScopedSyntheticCredential client_cred(store, cfg.security.credential_name, "synth_client_secret_token_123");
    ScopedSyntheticCredential admin_cred(store, cfg.security.admin_credential_name, "synth_admin_secret_token_456");

    gateway::Authenticator auth(cfg, store);

    // Missing Authorization header
    {
        httplib::Request req;
        auto err = auth.authenticate_client(req);
        assert(err.has_value() && err->http_status == 401 && err->code == "invalid_client_token");
    }

    // Duplicate Authorization headers -> rejected
    {
        httplib::Request req;
        req.headers.emplace("Authorization", "Bearer synth_client_secret_token_123");
        req.headers.emplace("Authorization", "Bearer synth_client_secret_token_123");
        auto err = auth.authenticate_client(req);
        assert(err.has_value() && err->http_status == 401 && err->code == "invalid_client_token");
    }

    // Wrong scheme or missing space
    {
        httplib::Request req;
        req.headers.emplace("Authorization", "Basic synth_client_secret_token_123");
        auto err = auth.authenticate_client(req);
        assert(err.has_value() && err->http_status == 401 && err->code == "invalid_client_token");
    }

    // Trailing whitespace in token is not trimmed -> rejected
    {
        httplib::Request req;
        req.headers.emplace("Authorization", "Bearer synth_client_secret_token_123 ");
        auto err = auth.authenticate_client(req);
        assert(err.has_value() && err->http_status == 401 && err->code == "invalid_client_token");
    }

    // Valid client token (exact and case-insensitive scheme "bearer ")
    {
        httplib::Request req1;
        req1.headers.emplace("Authorization", "Bearer synth_client_secret_token_123");
        assert(!auth.authenticate_client(req1).has_value());

        httplib::Request req2;
        req2.headers.emplace("Authorization", "bearer synth_client_secret_token_123");
        assert(!auth.authenticate_client(req2).has_value());
    }

    // Admin authentication is required even if client_auth is false, and uses admin_credential_name
    {
        gateway::GatewayConfig cfg_no_client = cfg;
        cfg_no_client.security.client_auth = false;
        gateway::Authenticator auth_no_client(cfg_no_client, store);

        httplib::Request req_missing;
        auto err1 = auth_no_client.authenticate_admin(req_missing);
        assert(err1.has_value() && err1->http_status == 401 && err1->code == "invalid_admin_token");

        httplib::Request req_wrong;
        req_wrong.headers.emplace("Authorization", "Bearer synth_client_secret_token_123");
        auto err2 = auth_no_client.authenticate_admin(req_wrong);
        assert(err2.has_value() && err2->http_status == 401 && err2->code == "invalid_admin_token");

        httplib::Request req_ok;
        req_ok.headers.emplace("Authorization", "Bearer synth_admin_secret_token_456");
        assert(!auth_no_client.authenticate_admin(req_ok).has_value());
    }
}

} // namespace

void test_auth_and_secrets() {
    // 1. Secret move and clear
    {
        gateway::Secret s1("super_secret_token_123");
        assert(s1.view() == "super_secret_token_123");
        assert(!s1.empty());

        gateway::Secret s2 = std::move(s1);
        assert(s2.view() == "super_secret_token_123");
        assert(s1.empty());
    }

    // 2. constant_time_equals
    assert(gateway::constant_time_equals("hello", "hello"));
    assert(!gateway::constant_time_equals("hello", "world"));
    assert(!gateway::constant_time_equals("hello", "hell"));
    assert(!gateway::constant_time_equals("hell", "hello"));
    assert(gateway::constant_time_equals("", ""));

    // 3. Secure token generation: 32 bytes -> Base64URL 43 characters
    std::string token = gateway::CredentialStore::generate_secure_token();
    assert(token.size() == 43);
    for (char c : token) {
        bool valid_b64url = std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
        assert(valid_b64url);
    }

    // 4. Target name and env var name formats
    gateway::CredentialStore store("CloudLLM", true);
    assert(store.format_target_name("openrouter", "default") == "CloudLLM/openrouter|default");
    assert(gateway::CredentialStore::format_env_var_name("openrouter", "default") == "LLM_GATEWAY_KEY_OPENROUTER_DEFAULT");

    // 5. Authenticator unit tests
    test_authenticator_unit();

    std::cout << "[PASS] Auth and Secret unit tests.\n";
}

void test_sec_l01_constant_time_boundaries() {
    using gateway::constant_time_equals;
    assert(constant_time_equals("", ""));
    assert(constant_time_equals("abc", "abc"));
    assert(!constant_time_equals("abc", "abd"));
    assert(!constant_time_equals("abc", "ab"));
    assert(!constant_time_equals("", "a"));
    // Length difference of exactly 256 / 512 with NUL padding must not compare equal.
    const std::string base = "token";
    std::string padded256 = base + std::string(256, '\0');
    std::string padded512 = base + std::string(512, '\0');
    assert(!constant_time_equals(base, padded256));
    assert(!constant_time_equals(padded256, base));
    assert(!constant_time_equals(base, padded512));
    assert(!constant_time_equals(padded256, padded512));
    assert(constant_time_equals(padded256, std::string(padded256)));
    std::cout << "[PASS] SEC-L01 constant_time_equals boundaries.\n";
}

int main() {
    test_auth_and_secrets();
    test_sec_l01_constant_time_boundaries();
    return 0;
}
