#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <functional>

namespace gateway {

class ShutdownCoordinator {
public:
    using ShutdownAction = std::function<void()>;

    class RequestGuard {
    public:
        RequestGuard() noexcept = default;
        explicit RequestGuard(ShutdownCoordinator* coordinator) noexcept
            : coordinator_(coordinator) {}

        RequestGuard(const RequestGuard&) = delete;
        RequestGuard& operator=(const RequestGuard&) = delete;

        RequestGuard(RequestGuard&& other) noexcept
            : coordinator_(other.coordinator_) {
            other.coordinator_ = nullptr;
        }

        RequestGuard& operator=(RequestGuard&& other) noexcept {
            if (this != &other) {
                release();
                coordinator_ = other.coordinator_;
                other.coordinator_ = nullptr;
            }
            return *this;
        }

        ~RequestGuard() noexcept {
            release();
        }

        void release() noexcept {
            if (coordinator_) {
                auto* c = coordinator_;
                coordinator_ = nullptr;
                c->leave_request();
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return coordinator_ != nullptr;
        }

    private:
        ShutdownCoordinator* coordinator_ = nullptr;
    };

    explicit ShutdownCoordinator(ShutdownAction custom_action = nullptr)
        : shutdown_action_(std::move(custom_action)) {}

    ~ShutdownCoordinator() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            destructor_cancelled_ = true;
            cv_.notify_all();
        }
        if (shutdown_thread_.joinable()) {
            shutdown_thread_.join();
        }
    }

    void set_shutdown_action(ShutdownAction action) {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_action_ = std::move(action);
    }

    [[nodiscard]] bool is_accepting() const noexcept {
        return accepting_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool try_enter_request() noexcept {
        active_requests_.fetch_add(1, std::memory_order_seq_cst);
        if (!accepting_.load(std::memory_order_seq_cst)) {
            leave_request();
            return false;
        }
        return true;
    }

    [[nodiscard]] RequestGuard acquire_guard() noexcept {
        if (try_enter_request()) {
            return RequestGuard(this);
        }
        return RequestGuard();
    }

    void leave_request() noexcept {
        int prev = active_requests_.fetch_sub(1, std::memory_order_seq_cst);
        if (prev <= 0) {
            // Defensive clamp against any unpaired leave_request() call
            active_requests_.fetch_add(1, std::memory_order_seq_cst);
            return;
        }
        if (prev == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            cv_.notify_all();
        }
    }

    [[nodiscard]] int active_requests() const noexcept {
        return active_requests_.load(std::memory_order_acquire);
    }

    bool wait_for_drain(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this]() {
            return destructor_cancelled_ || active_requests_.load(std::memory_order_acquire) <= 0;
        });
    }

    bool trigger_shutdown(std::chrono::milliseconds graceful_wait = std::chrono::milliseconds(5000)) {
        bool expected = false;
        if (!shutdown_triggered_.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
            // Already shutting down
            return false;
        }

        // Stop accepting new requests
        accepting_.store(false, std::memory_order_seq_cst);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            cv_.notify_all();
        }

        // Spawn owned thread to wait for active requests to drain and then run shutdown action
        shutdown_thread_ = std::thread([this, graceful_wait]() {
            ShutdownAction action_copy;
            bool cancelled = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, graceful_wait, [this]() {
                    return destructor_cancelled_ || active_requests_.load(std::memory_order_acquire) <= 0;
                });
                cancelled = destructor_cancelled_;
                action_copy = shutdown_action_;
            }

            if (cancelled) {
                return;
            }

            if (action_copy) {
                action_copy();
                return;
            }

            // Give a tiny grace period for final response packet to be flushed
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ExitProcess(0);
        });

        return true;
    }

    [[nodiscard]] bool is_shutting_down() const noexcept {
        return shutdown_triggered_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> accepting_{true};
    std::atomic<bool> shutdown_triggered_{false};
    std::atomic<int> active_requests_{0};
    bool destructor_cancelled_ = false;
    ShutdownAction shutdown_action_;
    std::thread shutdown_thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace gateway
