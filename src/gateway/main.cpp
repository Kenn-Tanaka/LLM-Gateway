#include "gateway/app.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

int main(int argc, char* argv[]) {
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (!wargv || wargc <= 0) {
        return gateway::run_app(argc, argv);
    }

    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
        int u8len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        if (u8len > 1) {
            std::string s(static_cast<size_t>(u8len - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), u8len, nullptr, nullptr);
            utf8_args.push_back(std::move(s));
        } else {
            utf8_args.emplace_back();
        }
    }
    LocalFree(wargv);

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_args.size());
    for (auto& s : utf8_args) {
        utf8_argv.push_back(s.data());
    }
    return gateway::run_app(static_cast<int>(utf8_argv.size()), utf8_argv.data());
}
