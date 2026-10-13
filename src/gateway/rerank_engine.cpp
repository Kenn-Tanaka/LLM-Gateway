#include "gateway/rerank_engine.hpp"
#include "gateway/credential_store.hpp"
#include "gateway/logger.hpp"

#include <llama.h>
#include <filesystem>
#include <cmath>
#include <algorithm>

namespace gateway {

namespace {

std::vector<int32_t> tokenize_raw(const llama_model* model, const std::string& text, bool add_special, bool parse_special) {
    if (!model || text.empty()) return {};
    int n_alloc = static_cast<int>(text.size()) + 16;
    std::vector<llama_token> tokens(n_alloc);
    int n = llama_tokenize(model, text.data(), static_cast<int32_t>(text.size()),
                           tokens.data(), static_cast<int32_t>(tokens.size()),
                           add_special, parse_special);
    if (n < 0) {
        tokens.resize(-n);
        n = llama_tokenize(model, text.data(), static_cast<int32_t>(text.size()),
                           tokens.data(), static_cast<int32_t>(tokens.size()),
                           add_special, parse_special);
    }
    if (n <= 0) {
        return {};
    }
    tokens.resize(n);
    return tokens;
}

int estimate_test_pair_tokens(const std::string& query, const std::string& doc) {
    auto count_fn = [](const std::string& s) {
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
        return (std::max)(count, static_cast<int>((s.size() + 3) / 4));
    };
    return count_fn(query) + count_fn(doc) + 4;
}

} // namespace

RerankEngine::RerankEngine() = default;

RerankEngine::~RerankEngine() {
    cleanup();
}

void RerankEngine::cleanup_locked() {
    test_score_fn_ = nullptr;
    if (ctx_) {
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    if (model_) {
        llama_free_model(model_);
        model_ = nullptr;
    }
}

void RerankEngine::cleanup() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    cleanup_locked();
}

void RerankEngine::disable() {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    state_.store(LoadState::Disabled, std::memory_order_release);
    cleanup_locked();
}

void RerankEngine::enable_test_mode(std::string filename, int max_tokens, TestScoreFn fn) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    cleanup_locked();
    filename_ = std::move(filename);
    max_tokens_ = (std::max)(1, max_tokens);
    test_score_fn_ = std::move(fn);
    state_.store(LoadState::Ready, std::memory_order_release);
}

bool RerankEngine::compare_rerank_items(const RerankItem& a, const RerankItem& b) noexcept {
    const bool a_nan = std::isnan(a.relevance_score);
    const bool b_nan = std::isnan(b.relevance_score);
    if (a_nan != b_nan) {
        return !a_nan; // Non-NaN always precedes NaN
    }
    if (!a_nan && a.relevance_score != b.relevance_score) {
        return a.relevance_score > b.relevance_score;
    }
    return a.index < b.index;
}

void RerankEngine::sort_rerank_items(std::vector<RerankItem>& items) {
    std::stable_sort(items.begin(), items.end(), compare_rerank_items);
}

std::vector<int32_t> RerankEngine::format_rerank_token_ids(
    const std::vector<int32_t>& q_tokens,
    const std::vector<int32_t>& d_tokens,
    int32_t bos_token,
    int32_t eos_token,
    int32_t sep_token) {

    std::vector<int32_t> result;
    result.reserve(q_tokens.size() + d_tokens.size() + 4);
    if (bos_token >= 0) {
        result.push_back(bos_token);
    }
    result.insert(result.end(), q_tokens.begin(), q_tokens.end());
    if (eos_token >= 0) {
        result.push_back(eos_token);
    }
    if (sep_token >= 0) {
        result.push_back(sep_token);
    }
    result.insert(result.end(), d_tokens.begin(), d_tokens.end());
    if (eos_token >= 0) {
        result.push_back(eos_token);
    } else if (sep_token >= 0) {
        result.push_back(sep_token);
    }
    return result;
}

std::vector<int32_t> RerankEngine::format_rerank_tokens(
    const llama_model* model,
    const std::string& query,
    const std::string& doc) {

    if (!model) return {};

    llama_token bos = llama_token_bos(model);
    if (bos < 0) bos = llama_token_cls(model);
    const llama_token eos = llama_token_eos(model);
    const llama_token sep = llama_token_sep(model);

    // When the model defines EOS or SEP tokens (as in BERT / XLM-RoBERTa / BGE / Jina cross-encoders),
    // construct [BOS] query [EOS] [SEP] doc [EOS] (matching llama.cpp format_rerank).
    if (eos >= 0 || sep >= 0) {
        auto q_tokens = tokenize_raw(model, query, false, true);
        auto d_tokens = tokenize_raw(model, doc, false, true);
        return format_rerank_token_ids(q_tokens, d_tokens, bos, eos, sep);
    }

    // Fallback when no explicit EOS/SEP tokens exist in vocabulary
    std::string pair_text = query + "\n" + doc;
    auto tokens = tokenize_raw(model, pair_text, true, true);
    if (tokens.empty() && bos >= 0) {
        tokens.push_back(bos);
    }
    return tokens;
}

void RerankEngine::init(const ModelFileConfig& config) {
    std::lock_guard<std::mutex> lock(inference_mutex_);
    state_.store(LoadState::Loading, std::memory_order_release);
    cleanup_locked();

    std::filesystem::path fp = path_from_utf8(config.path);
    filename_ = path_to_utf8(fp.filename());

    std::error_code ec;
    if (!std::filesystem::is_regular_file(fp, ec)) {
        get_logger().error("RerankEngine: model file does not exist or is not a regular file: " + config.path);
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; // CPU inference only per spec

    model_ = llama_load_model_from_file(config.path.c_str(), mparams);
    if (!model_) {
        get_logger().error("RerankEngine: failed to load model from " + config.path);
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    if (llama_model_has_encoder(model_) && llama_model_has_decoder(model_)) {
        get_logger().error("RerankEngine: encoder-decoder models are not supported for reranking: " + config.path);
        llama_free_model(model_);
        model_ = nullptr;
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_threads = config.n_threads;
    cparams.n_threads_batch = config.n_threads;
    cparams.embeddings = true;
    cparams.pooling_type = LLAMA_POOLING_TYPE_RANK;
    // For non-causal (encoder/cross-encoder) models, ubatch size must equal batch size
    cparams.n_ubatch = cparams.n_batch;

    ctx_ = llama_new_context_with_model(model_, cparams);
    if (!ctx_) {
        get_logger().error("RerankEngine: failed to create llama_context for " + config.path);
        llama_free_model(model_);
        model_ = nullptr;
        state_.store(LoadState::Error, std::memory_order_release);
        return;
    }

    max_tokens_ = static_cast<int>((std::min)(llama_n_ctx(ctx_), llama_n_batch(ctx_)));
    if (max_tokens_ <= 0) {
        max_tokens_ = 2048;
    }

    state_.store(LoadState::Ready, std::memory_order_release);
    get_logger().info("RerankEngine: model ready (" + filename_ + ", max_tokens=" + std::to_string(max_tokens_) + ")");
}

std::variant<RerankResult, GatewayError> RerankEngine::compute_rerank(
    const std::string& query,
    const std::vector<std::string>& documents,
    size_t top_n,
    bool return_documents) {

    std::lock_guard<std::mutex> lock(inference_mutex_);

    LoadState st = state();
    if (st == LoadState::Disabled) return errors::embedded_engine_disabled();
    if (st == LoadState::Loading) return errors::embedded_loading();
    if (st == LoadState::Error || (!test_score_fn_ && (!model_ || !ctx_))) return errors::embedded_load_failed();

    if (query.empty()) {
        return errors::bad_request("Rerank query cannot be empty.");
    }
    if (query.size() > 16 * 1024 * 1024) {
        return errors::bad_request("Rerank query exceeds maximum allowed byte length.");
    }
    if (documents.empty()) {
        return errors::bad_request("Rerank documents cannot be empty.");
    }
    if (top_n < 1) {
        return errors::bad_request("Field 'top_n' must be at least 1.");
    }

    for (const auto& doc : documents) {
        if (doc.empty()) {
            return errors::bad_request("Rerank document text cannot be empty.");
        }
        if (doc.size() > 16 * 1024 * 1024) {
            return errors::bad_request("Rerank document exceeds maximum allowed byte length.");
        }
    }

    RerankResult result;
    result.id = "rerank-" + CredentialStore::generate_secure_token().substr(0, 16);

    std::vector<RerankItem> items;
    items.reserve(documents.size());

    int total_tokens = 0;

    for (size_t i = 0; i < documents.size(); ++i) {
        const auto& doc = documents[i];

        if (test_score_fn_) {
            int n_tokens = estimate_test_pair_tokens(query, doc);
            if (n_tokens > max_tokens_) {
                return errors::bad_request(
                    "Rerank input token length (" + std::to_string(n_tokens) +
                    ") exceeds context limit (" + std::to_string(max_tokens_) + ").");
            }
            total_tokens += n_tokens;

            auto maybe_logit = test_score_fn_(query, doc);
            if (!maybe_logit.has_value() || !std::isfinite(*maybe_logit)) {
                return errors::embedded_load_failed("Failed to extract valid rerank score from model.");
            }
            float logit = *maybe_logit;
            float score = 1.0f / (1.0f + std::exp(-logit));
            if (!std::isfinite(score)) {
                return errors::embedded_load_failed("Failed to compute finite rerank score.");
            }

            RerankItem item;
            item.index = static_cast<int>(i);
            item.relevance_score = score;
            if (return_documents) {
                item.document_text = doc;
            }
            items.push_back(std::move(item));
            continue;
        }

        std::vector<llama_token> tokens = format_rerank_tokens(model_, query, doc);
        int n_tokens = static_cast<int>(tokens.size());
        if (n_tokens <= 0) {
            return errors::bad_request("Failed to tokenize rerank input pair.");
        }
        if (n_tokens > max_tokens_) {
            return errors::bad_request(
                "Rerank input token length (" + std::to_string(n_tokens) +
                ") exceeds context limit (" + std::to_string(max_tokens_) + ").");
        }

        total_tokens += n_tokens;

        // Clear KV cache before each document pair so previous documents or requests never contaminate state
        llama_kv_cache_clear(ctx_);

        const bool is_encoder_only = llama_model_has_encoder(model_) && !llama_model_has_decoder(model_);

        llama_batch batch = llama_batch_init(n_tokens, 0, 1);
        for (int k = 0; k < n_tokens; ++k) {
            batch.token[k] = tokens[k];
            batch.pos[k] = static_cast<llama_pos>(k);
            batch.n_seq_id[k] = 1;
            batch.seq_id[k][0] = 0;
            batch.logits[k] = true;
        }
        batch.n_tokens = n_tokens;

        const int32_t rc = is_encoder_only ? llama_encode(ctx_, batch) : llama_decode(ctx_, batch);
        if (rc != 0) {
            llama_batch_free(batch);
            return errors::embedded_load_failed("Failed to evaluate tokens in rerank engine.");
        }

        const float* emb = llama_get_embeddings_seq(ctx_, 0);
        if (!emb) {
            emb = llama_get_embeddings_ith(ctx_, -1);
        }
        if (!emb) {
            emb = llama_get_embeddings(ctx_);
        }

        if (!emb || !std::isfinite(emb[0])) {
            llama_batch_free(batch);
            return errors::embedded_load_failed("Failed to extract valid rerank score from model.");
        }

        float logit = emb[0];
        float score = 1.0f / (1.0f + std::exp(-logit));
        llama_batch_free(batch);

        if (!std::isfinite(score)) {
            return errors::embedded_load_failed("Failed to compute finite rerank score.");
        }

        RerankItem item;
        item.index = static_cast<int>(i);
        item.relevance_score = score;
        if (return_documents) {
            item.document_text = doc;
        }
        items.push_back(std::move(item));
    }

    // Sort descending by relevance_score, and ascending by index on ties (strict weak ordering)
    sort_rerank_items(items);

    if (top_n > items.size()) {
        top_n = items.size();
    }
    items.resize(top_n);

    result.results = std::move(items);
    result.prompt_tokens = total_tokens;
    result.total_tokens = total_tokens;

    return result;
}

} // namespace gateway
