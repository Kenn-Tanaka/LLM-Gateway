#include "gateway/model_catalog.hpp"
#include "gateway/embedded_engine.hpp"
#include "gateway/model_router.hpp"
#include "gateway/logger.hpp"

#include <curl/curl.h>
#include <algorithm>
#include <set>

namespace gateway {

namespace {

std::optional<std::string> http_get_json(const std::string& provider_id, const std::string& url, const std::string& auth_token, const TimeoutsConfig& timeouts) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        get_logger().warn("ModelCatalog: curl_easy_init failed for provider '" + provider_id + "'");
        return std::nullopt;
    }

    std::string response;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    if (!auth_token.empty()) {
        std::string auth = "Authorization: Bearer " + auth_token;
        headers = curl_slist_append(headers, auth.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeouts.upstream_read_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeouts.upstream_connect_ms));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        auto* str = static_cast<std::string*>(userdata);
        size_t total = size * nmemb;
        str->append(ptr, total);
        return total;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        get_logger().warn("ModelCatalog: model discovery failed for provider '" + provider_id + "' (" + url + "): " + curl_easy_strerror(res));
        return std::nullopt;
    }
    if (status < 200 || status >= 300) {
        get_logger().warn("ModelCatalog: model discovery returned HTTP " + std::to_string(status) + " for provider '" + provider_id + "' (" + url + ")");
        return std::nullopt;
    }
    return response;
}

} // namespace

ModelCatalog::ModelCatalog(const GatewayConfig& config, const CredentialStore& store)
    : config_(config), store_(store) {}

std::optional<std::vector<std::string>> ModelCatalog::fetch_provider_models(const ProviderConfig& provider) const {
    if (!provider.base_url || !provider.model_discovery) {
        return {};
    }

    std::string url = *provider.base_url;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/models";

    std::string token;
    if (provider.authentication == Authentication::Bearer) {
        auto secret_opt = store_.get_provider_credential(provider.key_vendor, provider.key_slot);
        if (secret_opt) {
            token = std::string(secret_opt->view());
        } else {
            get_logger().warn("ModelCatalog: bearer credential missing for provider '" + provider.id + "' (" + provider.key_vendor + "|" + provider.key_slot + ")");
        }
    }

    auto body = http_get_json(provider.id, url, token, config_.timeouts);
    if (!token.empty()) {
        SecureZeroMemory(token.data(), token.size());
    }

    if (!body) return std::nullopt;

    std::vector<std::string> discovered;
    try {
        auto j = nlohmann::json::parse(*body);
        if (!j.contains("data") || !j["data"].is_array()) {
            get_logger().warn("ModelCatalog: invalid /models payload (missing 'data' array) from provider '" + provider.id + "'");
            return std::nullopt;
        }
        for (const auto& item : j["data"]) {
            if (item.contains("id") && item["id"].is_string()) {
                std::string mid = item["id"].get<std::string>();
                std::string canonical = provider.id + "/" + mid;
                if (ModelRouter::matches_allowlist(canonical, provider.allowlist)) {
                    discovered.push_back(std::move(canonical));
                }
            }
        }
    } catch (const std::exception& e) {
        get_logger().warn("ModelCatalog: failed to parse /models JSON from provider '" + provider.id + "': " + e.what());
        return std::nullopt;
    }

    return discovered;
}

std::variant<nlohmann::json, GatewayError> ModelCatalog::get_models_list(
    const EmbeddedEngine& embedded_engine) const {

    struct ModelEntry {
        std::string id;
        std::string owned_by = "gateway";
    };

    std::vector<ModelEntry> models;
    std::set<std::string> seen_ids;

    // 1. All aliases
    for (const auto& alias : config_.models) {
        if (seen_ids.insert(alias.alias).second) {
            models.push_back({alias.alias, "gateway"});
        }
    }

    // 2. Discovered models from external providers (with 3600s TTL cache)
    auto now = std::chrono::steady_clock::now();
    size_t attempted = 0;
    size_t succeeded = 0;
    for (const auto& provider : config_.providers) {
        if (!provider.model_discovery) continue;
        ++attempted;

        std::vector<std::string> provider_models;
        bool cached = false;
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = cache_.find(provider.id);
            if (config_.model_cache.enabled && it != cache_.end() &&
                std::chrono::duration_cast<std::chrono::seconds>(now - it->second.fetched_at).count() < config_.model_cache.ttl_sec) {
                provider_models = it->second.discovered_models;
                cached = true;
            }
        }

        if (!cached) {
            auto fetched = fetch_provider_models(provider);
            if (!fetched) continue;
            provider_models = std::move(*fetched);
            if (config_.model_cache.enabled) {
                std::lock_guard<std::mutex> lock(cache_mutex_);
                cache_[provider.id] = ProviderModelCache{std::chrono::steady_clock::now(), provider_models};
            }
        }
        ++succeeded;

        for (const auto& m : provider_models) {
            if (seen_ids.insert(m).second) {
                models.push_back({m, provider.id});
            }
        }
    }

    // 3. Embedded models if ready
    if (embedded_engine.embedding().state() == LoadState::Ready) {
        const auto* prov = config_.find_provider("embedded");
        std::vector<std::string> candidates = {"embedded/embedding_model"};
        std::string fn = embedded_engine.embedding().filename();
        if (!fn.empty()) {
            candidates.push_back("embedded/" + fn);
        }
        for (const auto& c : candidates) {
            if (!prov || ModelRouter::matches_allowlist(c, prov->allowlist)) {
                if (seen_ids.insert(c).second) {
                    models.push_back({c, "embedded"});
                }
            }
        }
    }

    if (embedded_engine.reranker().state() == LoadState::Ready) {
        const auto* prov = config_.find_provider("embedded");
        std::vector<std::string> candidates = {"embedded/reranker_model"};
        std::string fn = embedded_engine.reranker().filename();
        if (!fn.empty()) {
            candidates.push_back("embedded/" + fn);
        }
        for (const auto& c : candidates) {
            if (!prov || ModelRouter::matches_allowlist(c, prov->allowlist)) {
                if (seen_ids.insert(c).second) {
                    models.push_back({c, "embedded"});
                }
            }
        }
    }

    if (attempted > 0 && succeeded == 0 && models.empty()) return errors::upstream_connection_failed("All provider model discovery requests failed.");

    // Stable sort ascending by id
    std::stable_sort(models.begin(), models.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });

    nlohmann::json data_arr = nlohmann::json::array();
    for (const auto& m : models) {
        data_arr.push_back({
            {"id", m.id},
            {"object", "model"},
            {"created", 1700000000},
            {"owned_by", m.owned_by}
        });
    }

    return nlohmann::json{
        {"object", "list"},
        {"data", data_arr}
    };
}

} // namespace gateway
