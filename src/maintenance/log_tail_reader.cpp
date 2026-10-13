#include "maintenance/log_tail_reader.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>

namespace maintenance {

namespace {

// Upper bound of bytes consumed per read_new_lines() call (bounds memory/UI load).
constexpr size_t kMaxBytesPerPoll = 1048576;
// A single unterminated line longer than this is flushed as-is.
constexpr size_t kMaxPendingBytes = 65536;

HANDLE open_shared(const std::filesystem::path& p) {
    return CreateFileW(p.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

LogTailReader::LogTailReader(std::filesystem::path log_dir, std::filesystem::path fixed_file)
    : log_dir_(std::move(log_dir)), fixed_file_(std::move(fixed_file)) {}

LogTailReader::~LogTailReader() = default;

std::filesystem::path LogTailReader::find_latest_log_file() const {
    std::error_code ec;
    if (!fixed_file_.empty()) return std::filesystem::is_regular_file(fixed_file_, ec) ? fixed_file_ : std::filesystem::path{};
    if (!std::filesystem::exists(log_dir_, ec)) return {};

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(log_dir_, ec)) {
        if (entry.is_regular_file()) {
            std::string name = entry.path().filename().string();
            if (name.rfind("llm-gateway-", 0) == 0 && name.find(".log") != std::string::npos) {
                files.push_back(entry.path());
            }
        }
    }

    if (files.empty()) return {};

    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
        return a.filename().string() > b.filename().string();
    });

    return files.front();
}

std::vector<std::string> LogTailReader::split_complete_lines(std::string data) {
    // Operates on raw bytes split only at '\n' (0x0A never occurs inside a UTF-8
    // multi-byte sequence), so multi-byte characters are never cut. The trailing
    // unterminated segment stays in pending_ until its newline arrives.
    std::vector<std::string> lines;
    pending_.append(data);
    size_t start = 0;
    while (true) {
        size_t nl = pending_.find('\n', start);
        if (nl == std::string::npos) break;
        std::string line = pending_.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        start = nl + 1;
    }
    pending_.erase(0, start);
    if (pending_.size() > kMaxPendingBytes) {
        lines.push_back(std::move(pending_));
        pending_.clear();
    }
    return lines;
}

std::vector<std::string> LogTailReader::read_initial_tail(size_t max_bytes) {
    current_file_ = find_latest_log_file();
    last_offset_ = 0;
    pending_.clear();
    if (current_file_.empty()) return {};

    HANDLE hFile = open_shared(current_file_);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(hFile, &fsize)) {
        CloseHandle(hFile);
        return {};
    }

    int64_t start_pos = 0;
    if (static_cast<uint64_t>(fsize.QuadPart) > max_bytes) {
        start_pos = fsize.QuadPart - static_cast<int64_t>(max_bytes);
    }

    LARGE_INTEGER li_pos = {};
    li_pos.QuadPart = start_pos;
    SetFilePointerEx(hFile, li_pos, nullptr, FILE_BEGIN);

    size_t to_read = static_cast<size_t>(fsize.QuadPart - start_pos);
    std::string buffer(to_read, 0);
    DWORD bytes_read = 0;
    ReadFile(hFile, buffer.data(), static_cast<DWORD>(to_read), &bytes_read, nullptr);
    buffer.resize(bytes_read);
    last_offset_ = start_pos + static_cast<int64_t>(bytes_read);
    CloseHandle(hFile);

    if (start_pos > 0) {
        // We began mid-file: drop the leading partial line (possibly a split UTF-8 char).
        size_t nl = buffer.find('\n');
        if (nl == std::string::npos) return {};
        buffer.erase(0, nl + 1);
    }
    return split_complete_lines(std::move(buffer));
}

std::vector<std::string> LogTailReader::read_new_lines() {
    auto latest = find_latest_log_file();
    if (latest != current_file_) {
        // File rotated: start reading from new file
        return read_initial_tail();
    }

    if (current_file_.empty()) return {};

    HANDLE hFile = open_shared(current_file_);
    if (hFile == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER fsize = {};
    if (!GetFileSizeEx(hFile, &fsize)) {
        CloseHandle(hFile);
        return {};
    }

    if (fsize.QuadPart < last_offset_) {
        // File was truncated/replaced with a smaller one: restart from the beginning.
        last_offset_ = 0;
        pending_.clear();
    }

    if (fsize.QuadPart == last_offset_) {
        CloseHandle(hFile);
        return {};
    }

    LARGE_INTEGER li_pos = {};
    li_pos.QuadPart = last_offset_;
    SetFilePointerEx(hFile, li_pos, nullptr, FILE_BEGIN);

    size_t to_read = (std::min)(static_cast<size_t>(fsize.QuadPart - last_offset_), kMaxBytesPerPoll);
    std::string buffer(to_read, 0);
    DWORD bytes_read = 0;
    ReadFile(hFile, buffer.data(), static_cast<DWORD>(to_read), &bytes_read, nullptr);
    buffer.resize(bytes_read);
    last_offset_ += bytes_read;
    CloseHandle(hFile);

    return split_complete_lines(std::move(buffer));
}

} // namespace maintenance
