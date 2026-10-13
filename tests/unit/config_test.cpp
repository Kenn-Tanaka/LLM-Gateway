#include "gateway/config.hpp"
#include "gateway/error.hpp"
#include "gateway/rerank_engine.hpp"
#include <llama.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>

void test_config_validation() {
    // 1. Valid config
    nlohmann::json valid_j = {
        {"schema_version", "2.0"},
        {"listen", {{"host", "127.0.0.1"}, {"port", 8765}}},
        {"limits", {{"max_request_bytes", 10485760}}},
        {"timeouts", {{"upstream_connect_ms", 5000}, {"upstream_read_ms", 60000}}},
        {"logging", {{"level", "INFO"}, {"dir", "%LOCALAPPDATA%\\LLM-Gateway\\logs"}}},
        {"security", {
            {"client_auth", true},
            {"credential_name", "llm_gateway|default"},
            {"admin_credential_name", "llm_gateway|admin"},
            {"cors", {{"allowed_origins", {"http://localhost:5173"}}}}
        }},
        {"credentials", {{"service_name", "CloudLLM"}, {"environment_fallback", false}}},
        {"embedded_engine", {{"enabled", false}}},
        {"models", {
            {"test-model", {{"provider", "openrouter"}, {"model", "anthropic/claude-3"}}}
        }},
        {"providers", {
            {"openrouter", {
                {"base_url", "https://openrouter.ai/api/v1"},
                {"authentication", "bearer"},
                {"key_vendor", "openrouter"},
                {"key_slot", "default"},
                {"model_discovery", true},
                {"allowlist", {"openrouter/*"}}
            }}
        }}
    };

    auto cfg = gateway::parse_and_validate_config(valid_j, false);
    assert(cfg.schema_version == "2.0");
    assert(cfg.listen.port == 8765);
    assert(cfg.models.size() == 1);
    assert(cfg.providers.size() == 1);

    // 2. Unknown field rejection
    nlohmann::json unknown_j = valid_j;
    unknown_j["unknown_field"] = "bad";
    bool caught_unknown = false;
    try {
        gateway::parse_and_validate_config(unknown_j, false);
    } catch (const gateway::GatewayException& e) {
        caught_unknown = true;
    }
    assert(caught_unknown);

    // 3. Invalid schema_version rejection
    nlohmann::json bad_version_j = valid_j;
    bad_version_j["schema_version"] = "1.0";
    bool caught_ver = false;
    try {
        gateway::parse_and_validate_config(bad_version_j, false);
    } catch (const gateway::GatewayException& e) {
        caught_ver = true;
    }
    assert(caught_ver);

    // 4. Invalid port rejection
    nlohmann::json bad_port_j = valid_j;
    bad_port_j["listen"]["port"] = 80; // < 1024
    bool caught_port = false;
    try {
        gateway::parse_and_validate_config(bad_port_j, false);
    } catch (const gateway::GatewayException& e) {
        caught_port = true;
    }
    assert(caught_port);

    // 5. Alias with slash rejection
    nlohmann::json bad_alias_j = valid_j;
    bad_alias_j["models"] = {
        {"invalid/alias", {{"provider", "openrouter"}, {"model", "m"}}}
    };
    bool caught_alias = false;
    try {
        gateway::parse_and_validate_config(bad_alias_j, false);
    } catch (const gateway::GatewayException& e) {
        caught_alias = true;
    }
    assert(caught_alias);

    std::cout << "[PASS] Config validation unit tests.\n";
}

void test_h12_strict_types_and_ranges() {
    nlohmann::json base = {
        {"schema_version", "2.0"},
        {"listen", {{"host", "127.0.0.1"}, {"port", 8765}}},
        {"limits", {{"max_request_bytes", 10485760}}},
        {"timeouts", {{"upstream_connect_ms", 5000}, {"upstream_read_ms", 60000}, {"upstream_write_ms", 60000}}},
        {"logging", {{"level", "INFO"}, {"dir", "%LOCALAPPDATA%\\LLM-Gateway\\logs"}}},
        {"security", {
            {"client_auth", true},
            {"credential_name", "llm_gateway|default"},
            {"admin_credential_name", "llm_gateway|admin"},
            {"cors", {{"enabled", true}, {"allowed_origins", {"http://localhost:5173"}}}}
        }},
        {"credentials", {{"service_name", "CloudLLM"}, {"environment_fallback", false}}},
        {"embedded_engine", {{"enabled", false}}}
    };

    auto expect_bad_request = [&](const nlohmann::json& candidate, const char* label) {
        bool caught = false;
        try {
            (void)gateway::parse_and_validate_config(candidate, false);
        } catch (const gateway::GatewayException& e) {
            assert(e.error().http_status == 400);
            assert(e.error().code == "bad_request");
            caught = true;
        } catch (...) {
            assert(false && "Expected GatewayException(400), got unexpected exception type");
        }
        if (!caught) {
            std::cerr << "Failed to reject invalid config case: " << label << "\n";
            assert(false);
        }
    };

    // Float values in integer fields must be rejected (no silent truncation)
    {
        auto j = base;
        j["listen"]["port"] = 8765.5;
        expect_bad_request(j, "float listen.port");
    }
    {
        auto j = base;
        j["limits"]["max_request_bytes"] = 1048576.25;
        expect_bad_request(j, "float limits.max_request_bytes");
    }
    {
        auto j = base;
        j["timeouts"]["upstream_connect_ms"] = 1000.5;
        expect_bad_request(j, "float timeouts.upstream_connect_ms");
    }
    {
        auto j = base;
        j["timeouts"]["upstream_read_ms"] = 1000.5;
        expect_bad_request(j, "float timeouts.upstream_read_ms");
    }
    {
        auto j = base;
        j["embedded_engine"]["embedding_model"] = {{"path", "model.gguf"}, {"n_threads", 2.5}};
        expect_bad_request(j, "float n_threads");
    }
    // Wrong JSON types (string for bool, number for string, string for array) must throw GatewayException(400)
    {
        auto j = base;
        j["security"]["client_auth"] = "true";
        expect_bad_request(j, "string security.client_auth");
    }
    {
        auto j = base;
        j["logging"]["dir"] = 12345;
        expect_bad_request(j, "number logging.dir");
    }
    {
        auto j = base;
        j["security"]["cors"]["allowed_origins"] = "http://localhost:5173";
        expect_bad_request(j, "string allowed_origins");
    }

    std::cout << "[PASS] H-12 strict config type & range validation unit tests.\n";
}

void test_h03_h14_unicode_paths_and_missing_models() {
    // 1. UTF-8 <-> filesystem::path round-trip with Japanese, spaces, and emoji
    const std::string rel_utf8 = "llm_gw_設定 テスト 🧪_dir";
    std::filesystem::path base_dir = std::filesystem::temp_directory_path() / gateway::path_from_utf8(rel_utf8);
    std::filesystem::path cfg_file = base_dir / gateway::path_from_utf8("設定 ファイル 🧪.json");

    std::string roundtrip = gateway::path_to_utf8(cfg_file);
    assert(roundtrip.find("設定 ファイル 🧪.json") != std::string::npos);

    std::error_code ec;
    std::filesystem::create_directories(base_dir, ec);
    assert(!ec);

    // 2. Missing file with emoji path throws GatewayException with UTF-8 path (never std::system_error)
    {
        std::filesystem::path missing_file = base_dir / gateway::path_from_utf8("存在しない 🧪.json");
        bool caught = false;
        try {
            (void)gateway::load_config_from_file(missing_file);
        } catch (const gateway::GatewayException& e) {
            caught = true;
            assert(std::string(e.what()).find("存在しない 🧪.json") != std::string::npos);
        }
        assert(caught);
    }

    // 3. H-03: Config with enabled embedded_engine and missing GGUF model loads cleanly by default
    nlohmann::json j = {
        {"schema_version", "2.0"},
        {"listen", {{"host", "127.0.0.1"}, {"port", 8765}}},
        {"embedded_engine", {
            {"enabled", true},
            {"embedding_model", {{"path", "missing_モデル_🧪.gguf"}, {"n_threads", 1}}}
        }}
    };
    {
        // Write to Unicode/emoji path using wide path
        std::ofstream ofs(cfg_file, std::ios::binary);
        assert(ofs.is_open());
        ofs << j.dump(2);
    }

    std::string cfg_utf8 = gateway::path_to_utf8(cfg_file);
    std::filesystem::path resolved = gateway::resolve_config_path(cfg_utf8, false);
    assert(resolved == cfg_file);

    // Default load_config_from_file does not abort on missing GGUF model file
    auto loaded = gateway::load_config_from_file(resolved);
    assert(loaded.embedded_engine.enabled);
    assert(loaded.embedded_engine.embedding_model.has_value());
    assert(loaded.embedded_engine.embedding_model->path == "missing_モデル_🧪.gguf");

    // Explicit validate_model_files = true still checks regular file existence with UTF-8 path
    bool caught_missing_model = false;
    try {
        (void)gateway::load_config_from_file(resolved, true);
    } catch (const gateway::GatewayException& e) {
        caught_missing_model = true;
    }
    assert(caught_missing_model);

    std::filesystem::remove_all(base_dir, ec);
    std::cout << "[PASS] H-03 & H-14 Unicode/emoji paths and non-fatal missing model config tests.\n";
}

void test_h08_rerank_sorting_and_token_formatting() {
    // 1. Strict weak ordering on the WORK_HANDOVER.md counterexample:
    //    (score=0.0f, index=0), (score=0.75e-6f, index=1), (score=1.5e-6f, index=2)
    //    Previously, std::fabs(a.score - b.score) > 1e-6f violated transitivity of equivalence
    //    and caused permutation-dependent ordering / UB in std::sort.
    const std::vector<gateway::RerankItem> base_items = {
        {0, 0.0f},
        {1, 0.75e-6f},
        {2, 1.5e-6f}
    };

    // Check strict weak ordering axioms across all pairs
    for (const auto& x : base_items) {
        assert(!gateway::RerankEngine::compare_rerank_items(x, x)); // Irreflexivity
        for (const auto& y : base_items) {
            if (gateway::RerankEngine::compare_rerank_items(x, y)) {
                assert(!gateway::RerankEngine::compare_rerank_items(y, x)); // Asymmetry
            }
            for (const auto& z : base_items) {
                if (gateway::RerankEngine::compare_rerank_items(x, y) &&
                    gateway::RerankEngine::compare_rerank_items(y, z)) {
                    assert(gateway::RerankEngine::compare_rerank_items(x, z)); // Transitivity
                }
            }
        }
    }

    // Verify all 6 permutations of the 3 counterexample items sort to the exact same order:
    // index 2 (1.5e-6f), index 1 (0.75e-6f), index 0 (0.0f)
    std::vector<int> perm = {0, 1, 2};
    int perm_count = 0;
    do {
        std::vector<gateway::RerankItem> items = {
            base_items[static_cast<size_t>(perm[0])],
            base_items[static_cast<size_t>(perm[1])],
            base_items[static_cast<size_t>(perm[2])]
        };
        gateway::RerankEngine::sort_rerank_items(items);
        assert(items.size() == 3);
        assert(items[0].index == 2 && items[0].relevance_score == 1.5e-6f);
        assert(items[1].index == 1 && items[1].relevance_score == 0.75e-6f);
        assert(items[2].index == 0 && items[2].relevance_score == 0.0f);
        ++perm_count;
    } while (std::next_permutation(perm.begin(), perm.end()));
    assert(perm_count == 6);

    // 2. Exact score ties and NaN handling
    {
        const float nan_val = std::numeric_limits<float>::quiet_NaN();
        std::vector<gateway::RerankItem> tie_and_nan = {
            {4, nan_val},
            {2, 0.5f},
            {0, 0.5f},
            {3, nan_val},
            {1, 0.5f}
        };
        for (const auto& x : tie_and_nan) {
            assert(!gateway::RerankEngine::compare_rerank_items(x, x));
        }
        gateway::RerankEngine::sort_rerank_items(tie_and_nan);
        assert(tie_and_nan[0].index == 0 && tie_and_nan[0].relevance_score == 0.5f);
        assert(tie_and_nan[1].index == 1 && tie_and_nan[1].relevance_score == 0.5f);
        assert(tie_and_nan[2].index == 2 && tie_and_nan[2].relevance_score == 0.5f);
        assert(tie_and_nan[3].index == 3 && std::isnan(tie_and_nan[3].relevance_score));
        assert(tie_and_nan[4].index == 4 && std::isnan(tie_and_nan[4].relevance_score));
    }

    // 3. RerankEngine::format_rerank_token_ids:
    //    Standard BERT/BGE/RoBERTa rerank framing: [BOS] query [EOS] [SEP] doc [EOS]
    {
        std::vector<int32_t> q_tokens = {1001, 1002};
        std::vector<int32_t> d_tokens = {2001, 2002, 2003};
        auto framed = gateway::RerankEngine::format_rerank_token_ids(q_tokens, d_tokens, 101, 102, 102);
        std::vector<int32_t> expected = {101, 1001, 1002, 102, 102, 2001, 2002, 2003, 102};
        assert(framed == expected);

        // When only SEP is defined (BOS=101, EOS=-1, SEP=102)
        auto sep_only = gateway::RerankEngine::format_rerank_token_ids(q_tokens, d_tokens, 101, -1, 102);
        std::vector<int32_t> expected_sep = {101, 1001, 1002, 102, 2001, 2002, 2003, 102};
        assert(sep_only == expected_sep);
    }

    // 4. Live vocab check of RerankEngine::format_rerank_tokens against ggml-vocab-bert-bge.gguf if present
    {
        std::error_code ec;
        std::filesystem::path bge_vocab_path = "build/_deps/llama-src/models/ggml-vocab-bert-bge.gguf";
        if (!std::filesystem::exists(bge_vocab_path, ec)) {
            bge_vocab_path = "../build/_deps/llama-src/models/ggml-vocab-bert-bge.gguf";
        }
        if (std::filesystem::exists(bge_vocab_path, ec)) {
            llama_backend_init();
            llama_model_params mparams = llama_model_default_params();
            mparams.vocab_only = true;
            std::string path_u8 = gateway::path_to_utf8(bge_vocab_path);
            llama_model* model = llama_load_model_from_file(path_u8.c_str(), mparams);
            assert(model != nullptr);

            llama_token bos = llama_token_bos(model);
            if (bos < 0) bos = llama_token_cls(model);
            llama_token eos = llama_token_eos(model);
            llama_token sep = llama_token_sep(model);
            assert(bos >= 0 || sep >= 0);

            auto seq = gateway::RerankEngine::format_rerank_tokens(
                model, "what is panda", "giant panda is a bear species");
            assert(!seq.empty());
            if (bos >= 0) {
                assert(seq.front() == bos);
            }
            if (eos >= 0) {
                assert(seq.back() == eos);
            } else if (sep >= 0) {
                assert(seq.back() == sep);
            }
            // Verify separator token appears between query and document tokens
            if (sep >= 0) {
                auto it = std::find(seq.begin() + 1, seq.end() - 1, sep);
                assert(it != seq.end() - 1);
            }
            llama_free_model(model);
        }
    }

    std::cout << "[PASS] H-08 RerankEngine strict weak ordering & special token framing unit tests.\n";
}

int main() {
    test_config_validation();
    test_h12_strict_types_and_ranges();
    test_h03_h14_unicode_paths_and_missing_models();
    test_h08_rerank_sorting_and_token_formatting();
    return 0;
}
