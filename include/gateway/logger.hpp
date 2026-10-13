#pragma once

#include <string>
#include <mutex>
#include <filesystem>
#include <cstdint>

namespace gateway {

enum class LogLevel {
    Error,
    Warn,
    Info,
    Debug
};

struct LogEntry {
    LogLevel level = LogLevel::Info;
    std::string client_ip = "127.0.0.1";
    std::string method;
    std::string endpoint;
    int status = 200;
    int64_t latency_ms = 0;
    uint32_t win32_error = 0;
    std::string message;
};

class Logger {
public:
    Logger() = default;
    ~Logger() = default;

    void init(const std::string& log_dir_template, const std::string& level_str, const std::string& fixed_file = "");

    void log(const LogEntry& entry);
    void log(LogLevel level, const std::string& message, uint32_t win32_error = 0);

    void info(const std::string& message) { log(LogLevel::Info, message); }
    void warn(const std::string& message, uint32_t win32_error = 0) { log(LogLevel::Warn, message, win32_error); }
    void error(const std::string& message, uint32_t win32_error = 0) { log(LogLevel::Error, message, win32_error); }
    void debug(const std::string& message) { log(LogLevel::Debug, message); }

    [[nodiscard]] LogLevel level() const noexcept { return current_level_; }
    [[nodiscard]] std::filesystem::path log_dir() const noexcept { return log_dir_; }
    [[nodiscard]] uint64_t write_error_count() const noexcept;
    [[nodiscard]] std::string last_write_error() const;

private:
    mutable std::mutex mutex_;
    LogLevel current_level_ = LogLevel::Info;
    std::filesystem::path log_dir_;
    std::string current_date_str_;
    std::filesystem::path current_file_path_;
    bool fixed_file_ = false;
    uint64_t write_error_count_ = 0;
    std::string last_write_error_;

    void rotate_and_cleanup_locked(const std::string& date_str);
    void write_line_locked(const std::string& line);
};

// Global logger instance
Logger& get_logger();

} // namespace gateway
