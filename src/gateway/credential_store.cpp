#include "gateway/credential_store.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#include <bcrypt.h>

#include <cctype>
#include <vector>
#include <memory>
#include <stdexcept>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace gateway {

namespace {

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (req <= 0) return L"";
    std::wstring out(req, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), req);
    return out;
}

std::string to_utf8(const wchar_t* wide, DWORD wide_len) {
    if (!wide || wide_len == 0) return "";
    int req = WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, nullptr, 0, nullptr, nullptr);
    if (req <= 0) return "";
    std::string out(req, 0);
    WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, out.data(), req, nullptr, nullptr);
    return out;
}

bool is_valid_utf8(const unsigned char* data, size_t len) {
    // Check that no NUL bytes exist and utf8 is valid
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == 0) return false;
    }
    int req = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(data), static_cast<int>(len), nullptr, 0);
    return req > 0;
}

std::string base64url_encode(const unsigned char* data, size_t len) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string result;
    result.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    while (i < len) {
        size_t rem = len - i;
        uint32_t val = static_cast<uint32_t>(data[i]) << 16;
        if (rem > 1) val |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (rem > 2) val |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(alphabet[(val >> 18) & 0x3F]);
        result.push_back(alphabet[(val >> 12) & 0x3F]);
        if (rem > 1) {
            result.push_back(alphabet[(val >> 6) & 0x3F]);
        }
        if (rem > 2) {
            result.push_back(alphabet[val & 0x3F]);
        }
        i += 3;
    }
    return result;
}

} // namespace

CredentialStore::CredentialStore(std::string service_name, bool environment_fallback)
    : service_name_(std::move(service_name)), environment_fallback_(environment_fallback) {}

std::string CredentialStore::format_target_name(const std::string& vendor, const std::string& slot) const {
    return service_name_ + "/" + vendor + "|" + slot;
}

std::string CredentialStore::format_env_var_name(const std::string& vendor, const std::string& slot) {
    std::string out = "LLM_GATEWAY_KEY_";
    for (char c : vendor) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        } else {
            out.push_back('_');
        }
    }
    out.push_back('_');
    for (char c : slot) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        } else {
            out.push_back('_');
        }
    }
    return out;
}

std::optional<Secret> CredentialStore::read_wincred(const std::string& target_name) const {
    std::wstring wtarget = to_wide(target_name);
    if (wtarget.empty()) return std::nullopt;

    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(wtarget.c_str(), CRED_TYPE_GENERIC, 0, &pcred)) {
        return std::nullopt;
    }

    struct CredDeleter {
        void operator()(PCREDENTIALW p) const {
            if (p) CredFree(p);
        }
    };
    std::unique_ptr<CREDENTIALW, CredDeleter> cred_guard(pcred);

    if (!pcred->CredentialBlob || pcred->CredentialBlobSize == 0) {
        return std::nullopt;
    }

    if (!is_valid_utf8(pcred->CredentialBlob, pcred->CredentialBlobSize)) {
        return std::nullopt;
    }

    std::string secret_str(reinterpret_cast<const char*>(pcred->CredentialBlob), pcred->CredentialBlobSize);
    return Secret(std::move(secret_str));
}

std::optional<Secret> CredentialStore::get_provider_credential(const std::string& vendor, const std::string& slot) const {
    std::string target = format_target_name(vendor, slot);
    auto wincred_val = read_wincred(target);
    if (wincred_val) {
        return wincred_val;
    }

    if (environment_fallback_) {
        std::string env_name = format_env_var_name(vendor, slot);
        std::wstring wenv = to_wide(env_name);
        DWORD size = GetEnvironmentVariableW(wenv.c_str(), nullptr, 0);
        if (size > 0) {
            std::wstring wbuf(size, 0);
            DWORD copied = GetEnvironmentVariableW(wenv.c_str(), wbuf.data(), size);
            if (copied > 0 && copied < size) {
                wbuf.resize(copied);
                std::string utf8_val = to_utf8(wbuf.data(), copied);
                SecureZeroMemory(wbuf.data(), wbuf.size() * sizeof(wchar_t));
                if (!utf8_val.empty()) {
                    return Secret(std::move(utf8_val));
                }
            }
        }
    }

    return std::nullopt;
}

std::optional<Secret> CredentialStore::get_token_credential(const std::string& target_name_or_slot) const {
    // If target already contains '/', use directly; otherwise prefix service_name + "/"
    std::string full_target;
    if (target_name_or_slot.find('/') != std::string::npos) {
        full_target = target_name_or_slot;
    } else {
        full_target = service_name_ + "/" + target_name_or_slot;
    }
    return read_wincred(full_target);
}

bool CredentialStore::has_credential(const std::string& target_name) const {
    std::string full_target;
    if (target_name.find('/') != std::string::npos) {
        full_target = target_name;
    } else {
        full_target = service_name_ + "/" + target_name;
    }

    std::wstring wtarget = to_wide(full_target);
    if (wtarget.empty()) return false;

    PCREDENTIALW pcred = nullptr;
    if (!CredReadW(wtarget.c_str(), CRED_TYPE_GENERIC, 0, &pcred)) {
        return false;
    }
    CredFree(pcred);
    return true;
}

bool CredentialStore::set_credential(const std::string& target_name, std::string_view secret_utf8) const {
    std::string full_target;
    if (target_name.find('/') != std::string::npos) {
        full_target = target_name;
    } else {
        full_target = service_name_ + "/" + target_name;
    }

    std::wstring wtarget = to_wide(full_target);
    if (wtarget.empty()) return false;

    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(wtarget.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(secret_utf8.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(secret_utf8.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;

    return CredWriteW(&cred, 0) == TRUE;
}

bool CredentialStore::delete_credential(const std::string& target_name) const {
    std::string full_target;
    if (target_name.find('/') != std::string::npos) {
        full_target = target_name;
    } else {
        full_target = service_name_ + "/" + target_name;
    }

    std::wstring wtarget = to_wide(full_target);
    if (wtarget.empty()) return false;

    return CredDeleteW(wtarget.c_str(), CRED_TYPE_GENERIC, 0) == TRUE;
}

std::string CredentialStore::generate_secure_token() {
    unsigned char random_bytes[32];
    NTSTATUS status = BCryptGenRandom(nullptr, random_bytes, sizeof(random_bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
        throw std::runtime_error("BCryptGenRandom failed to generate random bytes.");
    }

    std::string token = base64url_encode(random_bytes, sizeof(random_bytes));
    SecureZeroMemory(random_bytes, sizeof(random_bytes));
    return token;
}

std::vector<std::string> CredentialStore::list_service_credentials() const {
    std::vector<std::string> list;
    std::wstring wfilter = to_wide(service_name_ + "/*");
    DWORD count = 0;
    PCREDENTIALW* pcreds = nullptr;

    if (CredEnumerateW(wfilter.c_str(), 0, &count, &pcreds)) {
        for (DWORD i = 0; i < count; ++i) {
            if (pcreds[i] && pcreds[i]->TargetName) {
                std::string target = to_utf8(pcreds[i]->TargetName, static_cast<DWORD>(wcslen(pcreds[i]->TargetName)));
                list.push_back(target);
            }
        }
        CredFree(pcreds);
    }
    return list;
}

} // namespace gateway
