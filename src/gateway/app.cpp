#include "gateway/app.hpp"
#include "gateway/config.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/logger.hpp"
#include "gateway/embedded_engine.hpp"
#include "gateway/shutdown_coordinator.hpp"
#include "gateway/http_server.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <intrin.h>

#include <iostream>
#include <memory>

#pragma comment(lib, "ws2_32.lib")

namespace gateway {

bool check_avx2_support() {
    int cpu_info[4];
    __cpuid(cpu_info, 0);
    int n_ids = cpu_info[0];
    if (n_ids < 7) return false;

    // Check OSXSAVE (bit 27 of ECX with EAX=1)
    __cpuid(cpu_info, 1);
    bool osxsave = (cpu_info[2] & (1 << 27)) != 0;
    if (!osxsave) return false;

    // Check XGETBV for XMM and YMM state (bits 1 and 2)
    unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;

    // Check AVX2 (bit 5 of EBX with EAX=7, ECX=0)
    int cpu_info7[4];
    __cpuidex(cpu_info7, 7, 0);
    return (cpu_info7[1] & (1 << 5)) != 0;
}

int parse_cli_args(int argc, char* argv[], CliOptions& options) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--version") {
            options.show_version = true;
        } else if (arg == "--no-embedded") {
            options.no_embedded = true;
        } else if (arg == "--config") {
            if (options.config_path.has_value()) {
                // Duplicate --config is prohibited per spec 7.1
                std::cerr << "Error: Duplicate --config argument.\n";
                return 2;
            }
            if (i + 1 >= argc) {
                // Missing value for --config
                std::cerr << "Error: Missing value for --config argument.\n";
                return 2;
            }
            options.config_path = argv[++i];
        } else {
            // Unknown argument -> exit code 2
            std::cerr << "Error: Unknown argument: " << arg << "\n";
            return 2;
        }
    }
    return 0;
}

int run_app(int argc, char* argv[]) {
    CliOptions options;
    int parse_res = parse_cli_args(argc, argv, options);
    if (parse_res != 0) {
        return parse_res; // 2
    }

    if (options.show_version) {
        std::cout << "llm-gateway version 2.0.0\n";
        return 0;
    }

    // 1. Check AVX2 instruction set
    if (!check_avx2_support()) {
        std::cerr << "Error: CPU does not meet the AVX2 instruction set requirement.\n";
        return 1;
    }

    // 2. Named Mutex for single instance check per user session
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\LLM_Gateway_SingleInstance_Mutex");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "Error: Another instance of LLM Gateway is already running in this session.\n";
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 3. Load configuration
    GatewayConfig config;
    std::filesystem::path cfg_path;
    try {
        bool is_dev = false;
#ifndef NDEBUG
        is_dev = true;
#endif
        cfg_path = resolve_config_path(options.config_path, is_dev);
        config = load_config_from_file(cfg_path, false);
    } catch (const std::exception& e) {
        std::string path_str = !cfg_path.empty() ? path_to_utf8(cfg_path) : options.config_path.value_or("(default)");
        std::string msg = "Configuration Error [" + path_str + "]: " + e.what();
        std::cerr << msg << "\n";
        get_logger().init(config.logging.dir, "ERROR", config.logging.file.value_or(""));
        get_logger().error(msg);
        CloseHandle(hMutex);
        return 1;
    }

    // 4. Initialize Logger
    get_logger().init(config.logging.dir, config.logging.level, config.logging.file.value_or(""));
    get_logger().info("LLM-Gateway starting up (PID=" + std::to_string(GetCurrentProcessId()) + ", config=" + path_to_utf8(cfg_path) + ")...");

    // 5. Initialize Credential Store and verify required credentials
    CredentialStore store(config.credentials.service_name, config.credentials.environment_fallback);
    if (config.security.client_auth) {
        if (!store.has_credential(config.security.credential_name)) {
            get_logger().warn("Client credential not found in WinCred: " + config.security.credential_name);
        }
    }
    if (!store.has_credential(config.security.admin_credential_name)) {
        get_logger().warn("Admin credential not found in WinCred: " + config.security.admin_credential_name);
    }

    // 6. Initialize Winsock
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        get_logger().error("Winsock initialization failed.", GetLastError());
        CloseHandle(hMutex);
        return 1;
    }

    // 7. Embedded Engine and Shutdown Coordinator
    EmbeddedEngine embedded_engine(config, options.no_embedded);
    ShutdownCoordinator shutdown_coordinator;

    HttpServer server(config, store, embedded_engine, shutdown_coordinator);

    // 8. Start embedded engine async loading worker if enabled
    if (config.embedded_engine.enabled && !options.no_embedded) {
        embedded_engine.start_async_load();
    }

    // 9. Start HTTP server (synchronous listen loop)
    bool started = server.start();
    if (!started) {
        get_logger().error("Failed to start HTTP server or bind to port.", WSAGetLastError());
        WSACleanup();
        CloseHandle(hMutex);
        return 1;
    }

    WSACleanup();
    CloseHandle(hMutex);
    get_logger().info("LLM-Gateway cleanly shut down.");
    return 0;
}

} // namespace gateway
