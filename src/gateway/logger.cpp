#include "gateway/logger.hpp"
#include "gateway/config.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>

namespace gateway {

namespace {

std::string get_current_iso8601_time_and_date(std::string* out_date) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t timer = std::chrono::system_clock::to_time_t(now);

    std::tm tm_buf;
    localtime_s(&tm_buf, &timer);

    // Compute timezone offset
    std::tm gm_buf;
    gmtime_s(&gm_buf, &timer);
    std::time_t local_sec = std::mktime(&tm_buf);
    std::time_t gm_sec = std::mktime(&gm_buf);
    int diff_min = static_cast<int>((local_sec - gm_sec) / 60);

    char sign = '+';
    if (diff_min < 0) {
        sign = '-';
        diff_min = -diff_min;
    }
    int tz_hour = diff_min / 60;
    int tz_min = diff_min % 60;

    char date_str[16];
    std::snprintf(date_str, sizeof(date_str), "%04d%02d%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday);
    if (out_date) {
        *out_date = date_str;
    }

    char time_str[64];
    std::snprintf(time_str, sizeof(time_str), "%04d-%02d-%02dT%02d:%02d:%02d.%03d%c%02d:%02d",
                  tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
                  tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
                  static_cast<int>(ms.count()),
                  sign, tz_hour, tz_min);
    return time_str;
}

const char* level_to_string(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::Error: return "ERROR";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Debug: return "DEBUG";
    }
    return "INFO";
}

LogLevel string_to_level(const std::string& str) {
    if (str == "ERROR") return LogLevel::Error;
    if (str == "WARN")  return LogLevel::Warn;
    if (str == "DEBUG") return LogLevel::Debug;
    return LogLevel::Info;
}

} // namespace

Logger& get_logger() {
    static Logger instance;
    return instance;
}

void Logger::init(const std::string& log_dir_template, const std::string& level_str, const std::string& fixed_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_level_ = string_to_level(level_str);
    write_error_count_ = 0;
    last_write_error_.clear();

    std::string expanded = expand_environment_variables(log_dir_template);
    log_dir_ = path_from_utf8(expanded);
    fixed_file_ = !fixed_file.empty();
    current_date_str_.clear();
    current_file_path_.clear();
    if (fixed_file_) {
        std::error_code abs_ec;
        current_file_path_ = std::filesystem::absolute(path_from_utf8(expand_environment_variables(fixed_file)), abs_ec);
        log_dir_ = current_file_path_.parent_path();
    }

    std::error_code ec;
    if (!log_dir_.empty()) {
        std::filesystem::create_directories(log_dir_, ec);
        if (ec && !std::filesystem::exists(log_dir_)) {
            ++write_error_count_;
            last_write_error_ = "Failed to create log directory: " + path_to_utf8(log_dir_);
            std::cerr << "[Logger Error] " << last_write_error_ << "\n";
        }
    }

    std::string today_date;
    get_current_iso8601_time_and_date(&today_date);
    rotate_and_cleanup_locked(today_date);
}

uint64_t Logger::write_error_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return write_error_count_;
}

std::string Logger::last_write_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_write_error_;
}

void Logger::rotate_and_cleanup_locked(const std::string& date_str) {
    if (fixed_file_) return; // Legacy logging.file is an append-only fixed file.
    if (current_date_str_ == date_str && !current_file_path_.empty()) {
        return;
    }

    current_date_str_ = date_str;
    std::string filename = "llm-gateway-" + date_str + ".log";
    current_file_path_ = log_dir_ / filename;

    // Cleanup files older than 7 days
    std::error_code ec;
    if (std::filesystem::exists(log_dir_, ec)) {
        std::vector<std::filesystem::path> log_files;
        for (const auto& entry : std::filesystem::directory_iterator(log_dir_, ec)) {
            std::error_code entry_ec;
            if (entry.is_regular_file(entry_ec)) {
                std::string stem = path_to_utf8(entry.path().filename());
                if (stem.rfind("llm-gateway-", 0) == 0 && stem.find(".log") != std::string::npos) {
                    log_files.push_back(entry.path());
                }
            }
        }

        // Sort descending by filename (which has YYYYMMDD)
        std::sort(log_files.begin(), log_files.end(), [](const auto& a, const auto& b) {
            return path_to_utf8(a.filename()) > path_to_utf8(b.filename());
        });

        // Retain latest 7 files, delete from 8th onward
        for (size_t i = 7; i < log_files.size(); ++i) {
            std::filesystem::remove(log_files[i], ec);
        }
    }
}

void Logger::write_line_locked(const std::string& line) {
    if (current_file_path_.empty()) return;

    std::ofstream ofs(current_file_path_, std::ios::app);
    if (!ofs.is_open()) {
        ++write_error_count_;
        last_write_error_ = "Failed to open log file: " + path_to_utf8(current_file_path_);
        if (write_error_count_ == 1) {
            std::cerr << "[Logger Error] " << last_write_error_ << "\n";
        }
        return;
    }
    ofs << line << "\n";
    ofs.flush();
    if (!ofs.good()) {
        ++write_error_count_;
        last_write_error_ = "Failed to write to log file: " + path_to_utf8(current_file_path_);
        if (write_error_count_ == 1) {
            std::cerr << "[Logger Error] " << last_write_error_ << "\n";
        }
    }
}

void Logger::log(const LogEntry& entry) {
    if (static_cast<int>(entry.level) > static_cast<int>(current_level_)) {
        return;
    }

    std::string today_date;
    std::string timestamp = get_current_iso8601_time_and_date(&today_date);

    std::ostringstream ss;
    ss << timestamp
       << " level=" << level_to_string(entry.level)
       << " client_ip=" << entry.client_ip;

    if (!entry.method.empty()) {
        ss << " method=" << entry.method;
    }
    if (!entry.endpoint.empty()) {
        ss << " endpoint=" << entry.endpoint;
    }
    if (entry.status > 0) {
        ss << " status=" << entry.status;
    }
    if (entry.latency_ms >= 0) {
        ss << " latency_ms=" << entry.latency_ms;
    }
    ss << " win32_error=" << entry.win32_error;
    if (!entry.message.empty()) {
        ss << " msg=\"" << entry.message << "\"";
    }

    std::string line = ss.str();

    std::lock_guard<std::mutex> lock(mutex_);
    rotate_and_cleanup_locked(today_date);
    write_line_locked(line);
}

void Logger::log(LogLevel level, const std::string& message, uint32_t win32_error) {
    LogEntry entry;
    entry.level = level;
    entry.message = message;
    entry.win32_error = win32_error;
    entry.status = 0;
    entry.latency_ms = -1;
    log(entry);
}

} // namespace gateway
