#include "gateway/embedding_engine.hpp"
#include "gateway/logger.hpp"

#include <llama.h>
#include <filesystem>
#include <cmath>
#include <numeric>
#include <algorithm>

namespace gateway {

namespace {

int estimate_test_tokens(const std::string& s) {
    if (s.empty()) return 0;
    int count = 1;
    bool in_space = false;
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!in_space) {
                ++count;
                in_space = true;
            }
        } else {
            in_space = false;
        }
    }
    return std::max(count, static_cast<int>((s.size() + 3) / 4));
}

} // namespace

EmbeddingEngine::EmbeddingEngine() = default;

EmbeddingEngine::~EmbeddingEngine() {
    cleanup();
}

void EmbeddingEngine::cleanup_locked() {
    test_embed_fn_ = nullptr;
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        llama_free_model(model_);
        model_ = nullptr;
    }
}

void EmbeddingEngine::cleanup() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    cleanup_locked();
}

void EmbeddingEngine::disable() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    state_.store(LoadState::Disabled, std::memory_order_release);
    cleanup_locked();
}

void EmbeddingEngine::enable_test_mode(std::string filename, int dimensions, int max_tokens, TestEmbedFn fn) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    cleanup_locked();
    filename_ = std::move(filename);
    dimensions_ = std::max(1, dimensions);
    max_tokens_ = std::max(1, max_tokens);
    test_embed_fn_ = std::move(fn);
    state_.store(LoadState::Ready, std::memory_order_release);
}

void EmbeddingEngine::init(const ModelFileConfig& config) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    state_.store(LoadState::Loading, std::memory_order_release);
    cleanup_locked();

    std::filesystem::path fp = path_from_utf8(config.path);
    filename_ = path_to_utf8(fp.filename());

    std::error_code ec;
    if (!std::filesystem::is_regular_file(fp, ec)) {
        get_logger().error("EmbeddingEngine: model file does not exist or is not a regular file: " + config.path);
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; // CPU inference only per spec

    model_ = llama_load_model_from_file(config.path.c_str(), mparams);
    if (!model_) {
        get_logger().error("EmbeddingEngine: failed to load model from " + config.path);
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    if (llama_model_has_encoder(model_) && llama_model_has_decoder(model_)) {
        get_logger().error("EmbeddingEngine: encoder-decoder models are not supported for embeddings: " + config.path);
        llama_free_model(model_);
        model_ = nullptr;
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    dimensions_ = llama_n_embd(model_);
    if (dimensions_ <= 0) {
        get_logger().error("EmbeddingEngine: invalid embedding dimension for " + config.path);
        llama_free_model(model_);
        model_ = nullptr;
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_threads = config.n_threads;
    cparams.n_threads_batch = config.n_threads;
    cparams.embeddings = true;
    // For non-causal (encoder/BERT) models, ubatch size must equal batch size
    cparams.n_ubatch = cparams.n_batch;

    ctx_ = llama_new_context_with_model(model_, cparams);
    if (!ctx_) {
        get_logger().error("EmbeddingEngine: failed to create llama_context for " + config.path);
        llama_free_model(model_);
        model_ = nullptr;
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    max_tokens_ = static_cast<int>(std::min(llama_n_ctx(ctx_), llama_n_batch(ctx_)));
    if (max_tokens_ <= 0) {
        max_tokens_ = 2048;
    }

    state_.store(LoadState::Ready, std::memory_order_release);
    get_logger().info("EmbeddingEngine: model ready (" + filename_ + ", dim=" + std::to_string(dimensions_) + ", max_tokens=" + std::to_string(max_tokens_) + ")");
}

std::variant<EmbeddingResult, GatewayError> EmbeddingEngine::compute_embeddings(
    const std::vector<std::string>& texts,
    std::optional<int> requested_dimensions) {

    std::lock_guard<std::mutex> lock(inference_mutex_);

    LoadState st = state();
    if (st == LoadState::Disabled) return errors::embedded_engine_disabled();
    if (st == LoadState::Loading) return errors::embedded_loading();
    if (st == LoadState::Error || (!test_embed_fn_ && (!model_ || !ctx_))) return errors::embedded_load_failed();

    if (texts.empty()) {
        return errors::bad_request("Embedding input list cannot be empty.");
    }

    if (requested_dimensions.has_value() && *requested_dimensions != dimensions_) {
        return errors::unsupported_dimensions();
    }

    EmbeddingResult result;
    result.embeddings.reserve(texts.size());

    int total_tokens = 0;

    for (const auto& text : texts) {
        if (text.empty()) {
            return errors::bad_request("Embedding text input cannot be empty.");
        }
        if (text.size() > 16 * 1024 * 1024) {
            return errors::bad_request("Embedding text input exceeds maximum allowed byte length.");
        }

        if (test_embed_fn_) {
            int n_tokens = estimate_test_tokens(text);
            if (n_tokens > max_tokens_) {
                return errors::bad_request(
                    "Embedding input token length (" + std::to_string(n_tokens) +
                    ") exceeds context limit (" + std::to_string(max_tokens_) + ").");
            }
            total_tokens += n_tokens;

            auto maybe_vec = test_embed_fn_(text);
            if (!maybe_vec.has_value() || static_cast<int>(maybe_vec->size()) != dimensions_) {
                return errors::embedded_load_failed("Failed to extract embeddings.");
            }
            std::vector<float> vec = std::move(*maybe_vec);
            float norm = 0.0f;
            for (float v : vec) {
                if (!std::isfinite(v)) {
                    return errors::embedded_load_failed("Model produced non-finite embedding values.");
                }
                norm += v * v;
            }
            norm = std::sqrt(norm);
            if (norm > 1e-12f) {
                for (float& v : vec) v /= norm;
            }
            result.embeddings.push_back(std::move(vec));
            continue;
        }

        // Tokenize
        int n_tokens_alloc = static_cast<int>(text.size()) + 16;
        std::vector<llama_token> tokens(n_tokens_alloc);

        int n_tokens = llama_tokenize(model_, text.data(), static_cast<int32_t>(text.size()),
                                      tokens.data(), static_cast<int32_t>(tokens.size()),
                                      true, true);
        if (n_tokens < 0) {
            tokens.resize(-n_tokens);
            n_tokens = llama_tokenize(model_, text.data(), static_cast<int32_t>(text.size()),
                                      tokens.data(), static_cast<int32_t>(tokens.size()),
                                      true, true);
        }

        if (n_tokens <= 0) {
            llama_token bos = llama_token_bos(model_);
            if (bos < 0) bos = llama_token_cls(model_);
            if (bos < 0) {
                return errors::bad_request("Failed to tokenize embedding input text.");
            }
            tokens = {bos};
            n_tokens = 1;
        } else {
            tokens.resize(n_tokens);
        }

        if (n_tokens > max_tokens_) {
            return errors::bad_request(
                "Embedding input token length (" + std::to_string(n_tokens) +
                ") exceeds context limit (" + std::to_string(max_tokens_) + ").");
        }

        total_tokens += n_tokens;

        // Clear KV cache before each input so consecutive or batched inputs never leak state
        llama_kv_cache_clear(ctx_);

        const enum llama_pooling_type pooling_type = llama_pooling_type(ctx_);
        const bool is_encoder_only = llama_model_has_encoder(model_) && !llama_model_has_decoder(model_);

        // Build llama_batch
        llama_batch batch = llama_batch_init(n_tokens, 0, 1);
        for (int i = 0; i < n_tokens; ++i) {
            batch.token[i] = tokens[i];
            batch.pos[i] = static_cast<llama_pos>(i);
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = (pooling_type == LLAMA_POOLING_TYPE_NONE) ? (i == n_tokens - 1) : true;
        }
        batch.n_tokens = n_tokens;

        const int32_t rc = is_encoder_only ? llama_encode(ctx_, batch) : llama_decode(ctx_, batch);
        if (rc != 0) {
            llama_batch_free(batch);
            return errors::embedded_load_failed("Failed to evaluate tokens in EmbeddingEngine.");
        }

        const float* emb = nullptr;
        if (pooling_type == LLAMA_POOLING_TYPE_NONE) {
            emb = llama_get_embeddings_ith(ctx_, -1);
            if (!emb) {
                emb = llama_get_embeddings(ctx_);
            }
        } else {
            emb = llama_get_embeddings_seq(ctx_, 0);
            if (!emb) {
                emb = llama_get_embeddings_ith(ctx_, -1);
            }
            if (!emb) {
                emb = llama_get_embeddings(ctx_);
            }
        }

        if (!emb) {
            llama_batch_free(batch);
            return errors::embedded_load_failed("Failed to extract embeddings.");
        }

        // Copy, validate finite values, and L2-normalize embedding
        std::vector<float> vec(emb, emb + dimensions_);
        float norm = 0.0f;
        for (float v : vec) {
            if (!std::isfinite(v)) {
                llama_batch_free(batch);
                return errors::embedded_load_failed("Model produced non-finite embedding values.");
            }
            norm += v * v;
        }
        norm = std::sqrt(norm);
        if (norm > 1e-12f) {
            for (float& v : vec) v /= norm;
        }

        result.embeddings.push_back(std::move(vec));
        llama_batch_free(batch);
    }

    result.prompt_tokens = total_tokens;
    result.total_tokens = total_tokens;

    return result;
}

} // namespace gateway
