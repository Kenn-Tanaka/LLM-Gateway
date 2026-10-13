#pragma once

#include "gateway/config.hpp"
#include "gateway/error.hpp"

#include <string>
#include <vector>
#include <optional>
#include <variant>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

#include <functional>

struct llama_model;
struct llama_context;

namespace gateway {

enum class LoadState {
    Disabled,
    Loading,
    Ready,
    Error
};

inline const char* load_state_to_string(LoadState st) {
    switch (st) {
        case LoadState::Disabled: return "disabled";
        case LoadState::Loading:  return "loading";
        case LoadState::Ready:    return "ready";
        case LoadState::Error:    return "error";
    }
    return "disabled";
}

struct EmbeddingResult {
    std::vector<std::vector<float>> embeddings;
    int prompt_tokens = 0;
    int total_tokens = 0;
};

class EmbeddingEngine {
public:
    using TestEmbedFn = std::function<std::optional<std::vector<float>>(const std::string& text)>;

    EmbeddingEngine();
    ~EmbeddingEngine();

    void init(const ModelFileConfig& config);
    void disable();

    // Enables deterministic in-memory test mode without requiring a physical GGUF model file
    void enable_test_mode(std::string filename, int dimensions, int max_tokens, TestEmbedFn fn);

    [[nodiscard]] LoadState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int embedding_dimensions() const noexcept {
        return dimensions_;
    }

    [[nodiscard]] int max_tokens() const noexcept {
        return max_tokens_;
    }

    [[nodiscard]] std::string filename() const noexcept {
        return filename_;
    }

    // Computes embeddings for a batch of input texts. Thread-safe via dedicated mutex.
    [[nodiscard]] std::variant<EmbeddingResult, GatewayError> compute_embeddings(
        const std::vector<std::string>& texts,
        std::optional<int> requested_dimensions);

private:
    std::atomic<LoadState> state_{LoadState::Disabled};
    mutable std::mutex inference_mutex_;
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    int dimensions_ = 1024;
    int max_tokens_ = 2048;
    std::string filename_;
    TestEmbedFn test_embed_fn_;

    void cleanup_locked();
    void cleanup();
};

} // namespace gateway
