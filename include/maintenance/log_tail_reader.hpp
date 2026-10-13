#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace maintenance {

class LogTailReader {
public:
    explicit LogTailReader(std::filesystem::path log_dir, std::filesystem::path fixed_file = {});
    ~LogTailReader();

    // Finds latest log file and reads initial tail (up to 1MB)
    std::vector<std::string> read_initial_tail(size_t max_bytes = 1048576);

    // Reads newly appended lines since last read
    std::vector<std::string> read_new_lines();

    [[nodiscard]] std::filesystem::path current_file() const noexcept {
        return current_file_;
    }

private:
    std::filesystem::path log_dir_;
    std::filesystem::path fixed_file_;
    std::filesystem::path current_file_;
    int64_t last_offset_ = 0;
    std::string pending_; // unterminated trailing bytes carried to the next read

    [[nodiscard]] std::filesystem::path find_latest_log_file() const;
    std::vector<std::string> split_complete_lines(std::string data);
};

} // namespace maintenance
