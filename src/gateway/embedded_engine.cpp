#include "gateway/embedded_engine.hpp"
#include "gateway/logger.hpp"

namespace gateway {

EmbeddedEngine::EmbeddedEngine(const GatewayConfig& config, bool force_no_embedded)
    : config_(config) {
    if (force_no_embedded || !config.embedded_engine.enabled) {
        enabled_ = false;
        embedding_.disable();
        reranker_.disable();
    } else {
        enabled_ = true;
    }
}

EmbeddedEngine::~EmbeddedEngine() {
    if (load_worker_.joinable()) {
        load_worker_.join();
    }
}

void EmbeddedEngine::start_async_load() {
    if (!enabled_) return;

    load_worker_ = std::thread([this]() {
        get_logger().info("EmbeddedEngine: starting asynchronous model loading...");

        if (config_.embedded_engine.embedding_model) {
            embedding_.init(*config_.embedded_engine.embedding_model);
        } else {
            embedding_.disable();
        }

        if (config_.embedded_engine.reranker_model) {
            reranker_.init(*config_.embedded_engine.reranker_model);
        } else {
            reranker_.disable();
        }

        get_logger().info("EmbeddedEngine: asynchronous model loading completed (status=" + aggregate_state_string() + ")");
    });
}

LoadState EmbeddedEngine::aggregate_state() const noexcept {
    if (!enabled_) {
        return LoadState::Disabled;
    }

    LoadState emb_st = embedding_.state();
    LoadState rer_st = reranker_.state();

    if (emb_st == LoadState::Loading || rer_st == LoadState::Loading) {
        return LoadState::Loading;
    }

    bool has_active = false;
    bool all_ready = true;

    if (config_.embedded_engine.embedding_model) {
        has_active = true;
        if (emb_st != LoadState::Ready) all_ready = false;
    }
    if (config_.embedded_engine.reranker_model) {
        has_active = true;
        if (rer_st != LoadState::Ready) all_ready = false;
    }

    if (!has_active) {
        return LoadState::Disabled;
    }

    if (all_ready) {
        return LoadState::Ready;
    }

    if (emb_st == LoadState::Error || rer_st == LoadState::Error) {
        return LoadState::Error;
    }

    return LoadState::Disabled;
}

std::string EmbeddedEngine::aggregate_state_string() const noexcept {
    return load_state_to_string(aggregate_state());
}

} // namespace gateway
