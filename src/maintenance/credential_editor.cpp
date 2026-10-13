#include "maintenance/credential_editor.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <thread>
#include <chrono>

namespace maintenance {

namespace {

std::wstring to_wide_str(std::string_view utf8) {
    if (utf8.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(req, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), req);
    return out;
}

std::string get_clipboard_text() {
    if (!OpenClipboard(nullptr)) return "";
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (!hData) {
        CloseClipboard();
        return "";
    }
    wchar_t* pszText = static_cast<wchar_t*>(GlobalLock(hData));
    std::wstring wtext;
    if (pszText) {
        wtext = pszText;
        GlobalUnlock(hData);
    }
    CloseClipboard();

    int req = WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (req <= 0) return "";
    std::string out(req - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), -1, out.data(), req, nullptr, nullptr);
    return out;
}

} // namespace

CredentialEditor::CredentialEditor(const gateway::CredentialStore& store)
    : store_(store) {}

ProviderKeyStatus CredentialEditor::check_provider_key(const std::string& vendor, const std::string& slot) const {
    std::string target = store_.format_target_name(vendor, slot);
    ProviderKeyStatus status;
    status.vendor = vendor;
    status.slot = slot;
    status.is_registered = store_.has_credential(target);
    return status;
}

bool CredentialEditor::check_token_exists(const std::string& token_name) const {
    return store_.has_credential(token_name);
}

bool CredentialEditor::set_provider_key(const std::string& vendor, const std::string& slot, std::string_view secret) {
    std::string target = store_.format_target_name(vendor, slot);
    return store_.set_credential(target, secret);
}

bool CredentialEditor::set_token(const std::string& token_name, std::string_view secret) {
    return store_.set_credential(token_name, secret);
}

bool CredentialEditor::delete_provider_key(const std::string& vendor, const std::string& slot) {
    std::string target = store_.format_target_name(vendor, slot);
    return store_.delete_credential(target);
}

std::string CredentialEditor::generate_and_store_token(const std::string& token_name) {
    if (token_name.empty()) {
        return "";
    }
    std::string token = gateway::CredentialStore::generate_secure_token();
    if (token.empty()) {
        return "";
    }
    if (!store_.set_credential(token_name, token)) {
        SecureZeroMemory(token.data(), token.size());
        return "";
    }
    return token;
}

bool CredentialEditor::copy_to_clipboard_temporary(const std::string& text, uint32_t clear_after_sec) {
    if (text.empty()) return false;
    if (!OpenClipboard(nullptr)) return false;
    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    std::wstring wtext = to_wide_str(text);
    size_t bytes = (wtext.size() + 1) * sizeof(wchar_t);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hGlob) {
        CloseClipboard();
        return false;
    }

    void* locked = GlobalLock(hGlob);
    if (!locked) {
        GlobalFree(hGlob);
        CloseClipboard();
        return false;
    }
    memcpy(locked, wtext.c_str(), bytes);
    GlobalUnlock(hGlob);

    HANDLE placed = SetClipboardData(CF_UNICODETEXT, hGlob);
    if (!placed) {
        GlobalFree(hGlob);
        CloseClipboard();
        return false;
    }
    CloseClipboard();

    if (clear_after_sec > 0) {
        // Spawn thread to clear clipboard after clear_after_sec seconds if still identical
        std::thread([text, clear_after_sec]() {
            std::this_thread::sleep_for(std::chrono::seconds(clear_after_sec));
            std::string current = get_clipboard_text();
            if (current == text) {
                if (OpenClipboard(nullptr)) {
                    EmptyClipboard();
                    CloseClipboard();
                }
            }
        }).detach();
    }
    return true;
}

} // namespace maintenance
