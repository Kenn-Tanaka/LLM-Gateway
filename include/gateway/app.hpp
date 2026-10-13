#pragma once

#include <string>
#include <vector>
#include <optional>

namespace gateway {

struct CliOptions {
    std::optional<std::string> config_path;
    bool no_embedded = false;
    bool show_version = false;
};

// Validates AVX2 instruction set using CPUID and XGETBV
bool check_avx2_support();

// Parses command-line arguments. Returns 0 on success, 2 on CLI argument error.
int parse_cli_args(int argc, char* argv[], CliOptions& options);

// Runs the main application. Returns process exit code (0, 1, or 2).
int run_app(int argc, char* argv[]);

} // namespace gateway
