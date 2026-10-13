#pragma once

#include "gateway/config.hpp"
#include "gateway/embedding_engine.hpp"
#include "gateway/rerank_engine.hpp"

#include <memory>
#include <thread>
#include <atomic>

namespace gateway {

class EmbeddedEngine {
public:
    explicit EmbeddedEngine(const GatewayConfig& config, bool force_no_embedded = false);
    ~EmbeddedEngine();

    // Starts asynchronous model loading on background worker thread
    void start_async_load();

    // Aggregated state for /health endpoint
    [[nodiscard]] LoadState aggregate_state() const noexcept;
    [[nodiscard]] std::string aggregate_state_string() const noexcept;

    [[nodiscard]] EmbeddingEngine& embedding() noexcept { return embedding_; }
    [[nodiscard]] const EmbeddingEngine& embedding() const noexcept { return embedding_; }

    [[nodiscard]] RerankEngine& reranker() noexcept { return reranker_; }
    [[nodiscard]] const RerankEngine& reranker() const noexcept { return reranker_; }

    [[nodiscard]] bool is_enabled() const noexcept { return enabled_; }

private:
    bool enabled_ = false;
    const GatewayConfig& config_;
    EmbeddingEngine embedding_;
    RerankEngine reranker_;
    std::thread load_worker_;
    std::atomic<bool> worker_joined_{false};
};

} // namespace gateway
