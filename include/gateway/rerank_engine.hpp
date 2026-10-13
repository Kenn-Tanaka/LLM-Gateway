#pragma once

#include "gateway/config.hpp"
#include "gateway/error.hpp"
#include "gateway/embedding_engine.hpp"

#include <string>
#include <vector>
#include <variant>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>

#include <functional>
#include <cstdint>

struct llama_model;
struct llama_context;

namespace gateway {

struct RerankItem {
    int index;
    float relevance_score;
    std::string document_text;
};

struct RerankResult {
    std::string id;
    std::vector<RerankItem> results;
    int prompt_tokens = 0;
    int total_tokens = 0;
};

class RerankEngine {
public:
    // Callback returning raw logit (or std::nullopt to simulate extraction failure) for (query, doc)
    using TestScoreFn = std::function<std::optional<float>(const std::string& query, const std::string& doc)>;

    RerankEngine();
    ~RerankEngine();

    void init(const ModelFileConfig& config);
    void disable();

    // Enables deterministic in-memory test mode without requiring a physical GGUF model file
    void enable_test_mode(std::string filename, int max_tokens, TestScoreFn fn);

    // Strict weak ordering (total order): descending relevance_score (non-NaN before NaN), ascending index on ties
    [[nodiscard]] static bool compare_rerank_items(const RerankItem& a, const RerankItem& b) noexcept;
    static void sort_rerank_items(std::vector<RerankItem>& items);

    // Formats pre-tokenized query and document token IDs with model special tokens ([BOS]query[EOS][SEP]doc[EOS])
    [[nodiscard]] static std::vector<int32_t> format_rerank_token_ids(
        const std::vector<int32_t>& q_tokens,
        const std::vector<int32_t>& d_tokens,
        int32_t bos_token,
        int32_t eos_token,
        int32_t sep_token);

    // Formats and tokenizes a (query, doc) pair using model special tokens ([BOS]query[EOS][SEP]doc[EOS]) or fallback
    [[nodiscard]] static std::vector<int32_t> format_rerank_tokens(
        const llama_model* model,
        const std::string& query,
        const std::string& doc);

    [[nodiscard]] LoadState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int max_tokens() const noexcept {
        return max_tokens_;
    }

    [[nodiscard]] std::string filename() const noexcept {
        return filename_;
    }

    // Computes reranking scores for query and documents. Thread-safe via dedicated mutex.
    [[nodiscard]] std::variant<RerankResult, GatewayError> compute_rerank(
        const std::string& query,
        const std::vector<std::string>& documents,
        size_t top_n,
        bool return_documents);

private:
    std::atomic<LoadState> state_{LoadState::Disabled};
    mutable std::mutex inference_mutex_;
    llama_model* model_ = nullptr;
    llama_context* ctx_ = nullptr;
    int max_tokens_ = 2048;
    std::string filename_;
    TestScoreFn test_score_fn_;

    void cleanup_locked();
    void cleanup();
};

} // namespace gateway
