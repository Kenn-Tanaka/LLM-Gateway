#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <string_view>
#include <algorithm>
#include <utility>

namespace gateway {

class Secret {
public:
    Secret() = default;

    explicit Secret(std::string data) : data_(std::move(data)) {}

    ~Secret() {
        clear();
    }

    // No copy allowed
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;

    // Move allowed
    Secret(Secret&& other) noexcept : data_(std::move(other.data_)) {}

    Secret& operator=(Secret&& other) noexcept {
        if (this != &other) {
            clear();
            data_ = std::move(other.data_);
        }
        return *this;
    }

    void clear() noexcept {
        if (!data_.empty()) {
            SecureZeroMemory(data_.data(), data_.size());
            data_.clear();
        }
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return data_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return data_.empty();
    }

    [[nodiscard]] size_t size() const noexcept {
        return data_.size();
    }

private:
    std::string data_;
};

// Constant-time string comparison that minimizes timing leakage including length differences.
inline bool constant_time_equals(std::string_view a, std::string_view b) noexcept {
    const size_t max_len = (std::max)(a.size(), b.size());
    // Keep the full-width length difference (size_t) so that lengths differing by a
    // multiple of 256 are still detected (SEC-L01).
    size_t diff = a.size() ^ b.size();

    for (size_t i = 0; i < max_len; ++i) {
        const unsigned char ca = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned char cb = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<size_t>(ca ^ cb);
    }
    return diff == 0;
}

} // namespace gateway
