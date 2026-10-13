#include "gateway/config.hpp"
#include "gateway/error.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <regex>
#include <fstream>
#include <sstream>
#include <thread>
#include <set>
#include <algorithm>
#include <cctype>

namespace gateway {

namespace {

void check_unknown_keys(const nlohmann::json& obj, const std::set<std::string>& allowed_keys, const std::string& context) {
    if (!obj.is_object()) {
        throw GatewayException(400, "bad_request", context + " must be a JSON object.");
    }
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        if (allowed_keys.find(it.key()) == allowed_keys.end()) {
            throw GatewayException(400, "bad_request", "Unknown field in " + context + ": '" + it.key() + "'");
        }
    }
}

bool is_valid_provider_id(const std::string& id) {
    static const std::regex re("^[a-z0-9][a-z0-9_-]{0,63}$");
    return std::regex_match(id, re);
}

bool is_valid_alias_name(const std::string& name) {
    if (name.find('/') != std::string::npos) return false;
    static const std::regex re("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$");
    return std::regex_match(name, re);
}

bool is_valid_credential_name(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) return false; // Control characters prohibited
    }
    return true;
}

bool is_valid_url(const std::string& url) {
    // http or https, userinfo (@) forbidden, fragment (#) forbidden
    if (url.find('@') != std::string::npos) return false;
    if (url.find('#') != std::string::npos) return false;
    static const std::regex re("^https?://[^/?#]+.*$");
    return std::regex_match(url, re);
}

bool is_valid_origin(const std::string& origin) {
    // http(s)://host[:port], path/query/fragment/userinfo prohibited
    static const std::regex re("^https?://[a-zA-Z0-9.-]+(:[0-9]{1,5})?$");
    return std::regex_match(origin, re);
}

bool is_valid_allowlist_pattern(const std::string& pattern) {
    if (pattern.empty()) return false;
    if (pattern == "*") return true;

    // Check if '*' or '?' or regex characters exist
    size_t star_pos = pattern.find('*');
    if (star_pos != std::string::npos) {
        // Only allowed at the very end as "/*"
        if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == "/*" && star_pos == pattern.size() - 1) {
            // Check that no other '*' or '?' exists before it
            if (pattern.substr(0, pattern.size() - 2).find('*') != std::string::npos) return false;
            if (pattern.find('?') != std::string::npos) return false;
            return true;
        }
        return false;
    }
    if (pattern.find('?') != std::string::npos) return false;
    return true;
}

} // namespace

const ProviderConfig* GatewayConfig::find_provider(const std::string& provider_id) const {
    for (const auto& p : providers) {
        if (p.id == provider_id) return &p;
    }
    return nullptr;
}

const ModelAlias* GatewayConfig::find_alias(const std::string& alias_name) const {
    for (const auto& a : models) {
        if (a.alias == alias_name) return &a;
    }
    return nullptr;
}

std::filesystem::path path_from_utf8(std::string_view utf8_path) {
    if (utf8_path.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8_path.data(), static_cast<int>(utf8_path.size()), nullptr, 0);
    if (wlen <= 0) return std::filesystem::path(std::string(utf8_path));
    std::wstring wstr(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.data(), static_cast<int>(utf8_path.size()), wstr.data(), wlen);
    return std::filesystem::path(wstr);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::wstring wstr = path.wstring();
    if (wstr.empty()) return "";
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return "";
    std::string out(static_cast<size_t>(u8len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), out.data(), u8len, nullptr, nullptr);
    return out;
}

std::string expand_environment_variables(const std::string& input) {
    if (input.empty()) return "";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return input;
    std::wstring winput(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, winput.data(), wlen);

    DWORD req = ExpandEnvironmentStringsW(winput.c_str(), nullptr, 0);
    if (req == 0) return input;
    std::wstring wout(req, 0);
    ExpandEnvironmentStringsW(winput.c_str(), wout.data(), req);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, wout.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8len <= 0) return input;
    std::string out(u8len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wout.c_str(), -1, out.data(), u8len, nullptr, nullptr);
    return out;
}

std::filesystem::path resolve_config_path(const std::optional<std::string>& cli_config_path, bool is_development_mode) {
    std::error_code ec;
    if (cli_config_path && !cli_config_path->empty()) {
        std::filesystem::path p = path_from_utf8(*cli_config_path);
        if (std::filesystem::exists(p, ec)) return p;
        throw GatewayException(400, "bad_request", "Specified config file does not exist: " + *cli_config_path);
    }

    std::string expanded_appdata = expand_environment_variables("%LOCALAPPDATA%\\LLM-Gateway\\llm-gateway.json");
    std::filesystem::path default_path = path_from_utf8(expanded_appdata);
    if (std::filesystem::exists(default_path, ec)) {
        return default_path;
    }

    if (is_development_mode) {
        std::filesystem::path exe_dir = std::filesystem::current_path(ec);
        std::filesystem::path adjacent = exe_dir / "llm-gateway.json";
        if (std::filesystem::exists(adjacent, ec)) {
            return adjacent;
        }
    }

    // Default target path for creation/loading
    return default_path;
}

namespace {

int64_t integer_in_range(const nlohmann::json& value, int64_t minimum, int64_t maximum, const std::string& name) {
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(maximum))) {
        throw GatewayException(400, "bad_request", name + " must be an integer in range.");
    }
    const auto result = value.get<int64_t>();
    if (result < minimum || result > maximum) {
        throw GatewayException(400, "bad_request", name + " is out of range.");
    }
    return result;
}

std::string require_string(const nlohmann::json& value, const std::string& name) {
    if (!value.is_string()) {
        throw GatewayException(400, "bad_request", name + " must be a string.");
    }
    return value.get<std::string>();
}

bool require_bool(const nlohmann::json& value, const std::string& name) {
    if (!value.is_boolean()) {
        throw GatewayException(400, "bad_request", name + " must be a boolean.");
    }
    return value.get<bool>();
}

std::vector<std::string> require_string_array(const nlohmann::json& value, const std::string& name) {
    if (!value.is_array()) {
        throw GatewayException(400, "bad_request", name + " must be an array.");
    }
    std::vector<std::string> out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_string()) {
            throw GatewayException(400, "bad_request", name + "[" + std::to_string(i) + "] must be a string.");
        }
        out.push_back(value[i].get<std::string>());
    }
    return out;
}

// Only unversioned configurations use the legacy adapter. Versioned input remains strict.
nlohmann::json normalize_legacy_config(const nlohmann::json& input) {
    check_unknown_keys(input, {"listen", "security", "credentials", "proxy", "model_cache", "cors", "providers", "logging"}, "legacy configuration");
    auto j = input;
    j["schema_version"] = "2.0";
    j["embedded_engine"] = {{"enabled", false}};
    if (j.contains("listen")) {
        auto& listen = j["listen"];
        check_unknown_keys(listen, {"address", "port"}, "legacy listen");
        if (listen.contains("address")) { listen["host"] = listen["address"]; listen.erase("address"); }
    }
    const auto proxy = j.contains("proxy") ? j["proxy"] : nlohmann::json::object();
    check_unknown_keys(proxy, {"max_request_body_mb", "connect_timeout_sec", "read_timeout_sec", "write_timeout_sec"}, "legacy proxy");
    j["limits"] = {{"max_request_bytes", integer_in_range(proxy.value("max_request_body_mb", nlohmann::json(64)), 1, 256, "proxy.max_request_body_mb") * 1024 * 1024}};
    j["timeouts"] = {
        {"upstream_connect_ms", integer_in_range(proxy.value("connect_timeout_sec", nlohmann::json(10)), 1, 3600, "proxy.connect_timeout_sec") * 1000},
        {"upstream_read_ms", integer_in_range(proxy.value("read_timeout_sec", nlohmann::json(600)), 1, 3600, "proxy.read_timeout_sec") * 1000},
        {"upstream_write_ms", integer_in_range(proxy.value("write_timeout_sec", nlohmann::json(600)), 1, 3600, "proxy.write_timeout_sec") * 1000}
    };
    j.erase("proxy");
    if (j.contains("security")) check_unknown_keys(j["security"], {"client_auth", "credential_name", "admin_credential_name"}, "legacy security");
    else j["security"] = nlohmann::json::object();
    j["security"]["cors"] = j.contains("cors") ? j["cors"] : nlohmann::json{{"enabled", false}};
    j.erase("cors");
    if (!j.contains("logging")) j["logging"] = nlohmann::json::object();
    auto& logging = j["logging"];
    check_unknown_keys(logging, {"level", "file"}, "legacy logging");
    std::string level = logging.contains("level") ? require_string(logging["level"], "logging.level") : std::string("info");
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    logging["level"] = level;
    if (!logging.contains("file")) logging["file"] = "llm-gateway.log";
    if (j.contains("providers")) {
        if (!j["providers"].is_object()) throw GatewayException(400, "bad_request", "providers must be an object.");
        for (auto& [id, provider] : j["providers"].items()) {
            if (!provider.is_object()) throw GatewayException(400, "bad_request", "provider must be an object.");
            if (!provider.contains("model_discovery")) provider["model_discovery"] = true;
            if (provider.contains("authentication") && provider["authentication"].is_string() && provider["authentication"].get<std::string>() == "bearer" && !provider.contains("key_slot")) {
                provider["key_slot"] = "default";
            }
            if (provider.contains("allowlist")) {
                if (!provider["allowlist"].is_array()) throw GatewayException(400, "bad_request", "allowlist must be an array.");
                for (auto& rule : provider["allowlist"]) {
                    check_unknown_keys(rule, {"type", "pattern"}, "legacy allowlist rule");
                    if (!rule.contains("type") || !rule.contains("pattern")) {
                        throw GatewayException(400, "bad_request", "legacy allowlist rule requires type and pattern.");
                    }
                    const auto type = require_string(rule["type"], "allowlist.type");
                    const auto pattern = require_string(rule["pattern"], "allowlist.pattern");
                    if ((type != "glob" && type != "exact") || (type == "exact" && pattern.find_first_of("*?") != std::string::npos)) {
                        throw GatewayException(400, "bad_request", "Unsupported legacy allowlist rule.");
                    }
                    rule = pattern;
                }
            }
            // The old proxy appended the public /v1 route to an API-root URL.
            if (provider.contains("base_url") && provider["base_url"].is_string()) {
                auto url = provider["base_url"].get<std::string>();
                while (!url.empty() && url.back() == '/') url.pop_back();
                if (!url.ends_with("/v1")) url += "/v1";
                provider["base_url"] = url;
            }
        }
    }
    return j;
}

} // namespace

GatewayConfig parse_and_validate_config(const nlohmann::json& input, bool validate_model_files) {
    const bool legacy = input.is_object() && !input.contains("schema_version");
    const auto j = legacy ? normalize_legacy_config(input) : input;
    static const std::set<std::string> top_keys = {
        "schema_version", "listen", "limits", "timeouts", "logging",
        "security", "credentials", "embedded_engine", "models", "providers", "model_cache"
    };
    check_unknown_keys(j, top_keys, "root configuration");

    GatewayConfig config;
    config.loaded_from_legacy = legacy;

    // 1. schema_version
    if (!j.contains("schema_version") || !j["schema_version"].is_string()) {
        throw GatewayException(400, "bad_request", "Missing or invalid 'schema_version'.");
    }
    config.schema_version = j["schema_version"].get<std::string>();
    if (config.schema_version != "2.0") {
        throw GatewayException(400, "bad_request", "'schema_version' must be exactly '2.0'.");
    }

    // 2. listen
    if (j.contains("listen")) {
        const auto& l = j["listen"];
        static const std::set<std::string> listen_keys = {"host", "port"};
        check_unknown_keys(l, listen_keys, "listen");
        if (l.contains("host")) {
            config.listen.host = require_string(l["host"], "listen.host");
            if (config.listen.host != "127.0.0.1") {
                throw GatewayException(400, "bad_request", "listen.host must be exactly '127.0.0.1'.");
            }
        }
        if (l.contains("port")) {
            config.listen.port = static_cast<uint16_t>(integer_in_range(l["port"], 1024, 65535, "listen.port"));
        }
    }

    // 3. limits
    if (j.contains("limits")) {
        const auto& lm = j["limits"];
        static const std::set<std::string> limit_keys = {"max_request_bytes"};
        check_unknown_keys(lm, limit_keys, "limits");
        if (lm.contains("max_request_bytes")) {
            config.limits.max_request_bytes = static_cast<size_t>(integer_in_range(lm["max_request_bytes"], 1, 268435456, "limits.max_request_bytes"));
        }
    }

    // 4. timeouts
    if (j.contains("timeouts")) {
        const auto& t = j["timeouts"];
        static const std::set<std::string> timeout_keys = {"upstream_connect_ms", "upstream_read_ms", "upstream_write_ms"};
        check_unknown_keys(t, timeout_keys, "timeouts");
        if (t.contains("upstream_write_ms")) config.timeouts.upstream_write_ms = static_cast<uint32_t>(integer_in_range(t["upstream_write_ms"], 1, 3600000, "timeouts.upstream_write_ms"));
        if (t.contains("upstream_connect_ms")) {
            config.timeouts.upstream_connect_ms = static_cast<uint32_t>(integer_in_range(t["upstream_connect_ms"], 1, 3600000, "timeouts.upstream_connect_ms"));
        }
        if (t.contains("upstream_read_ms")) {
            config.timeouts.upstream_read_ms = static_cast<uint32_t>(integer_in_range(t["upstream_read_ms"], 1, 3600000, "timeouts.upstream_read_ms"));
        }
    }

    if (j.contains("model_cache")) {
        const auto& cache = j["model_cache"];
        check_unknown_keys(cache, {"enabled", "ttl_sec"}, "model_cache");
        if (cache.contains("enabled")) config.model_cache.enabled = require_bool(cache["enabled"], "model_cache.enabled");
        if (cache.contains("ttl_sec")) config.model_cache.ttl_sec = static_cast<uint32_t>(integer_in_range(cache["ttl_sec"], 0, 31536000, "model_cache.ttl_sec"));
    }

    // 5. logging
    if (j.contains("logging")) {
        const auto& log = j["logging"];
        static const std::set<std::string> log_keys = {"level", "dir", "file"};
        check_unknown_keys(log, log_keys, "logging");
        if (log.contains("file")) {
            const auto file = require_string(log["file"], "logging.file");
            if (file.empty() || file.find('\0') != std::string::npos) throw GatewayException(400, "bad_request", "logging.file must be a nonempty path.");
            config.logging.file = file;
        }
        if (log.contains("level")) {
            std::string lvl = require_string(log["level"], "logging.level");
            if (lvl != "ERROR" && lvl != "WARN" && lvl != "INFO" && lvl != "DEBUG") {
                throw GatewayException(400, "bad_request", "logging.level must be ERROR, WARN, INFO, or DEBUG.");
            }
            config.logging.level = lvl;
        }
        if (log.contains("dir")) {
            const auto dir = require_string(log["dir"], "logging.dir");
            if (dir.empty() || dir.find('\0') != std::string::npos) throw GatewayException(400, "bad_request", "logging.dir must be a nonempty path.");
            config.logging.dir = dir;
        }
    }

    // 6. security
    if (j.contains("security")) {
        const auto& sec = j["security"];
        static const std::set<std::string> sec_keys = {"client_auth", "credential_name", "admin_credential_name", "cors"};
        check_unknown_keys(sec, sec_keys, "security");
        if (sec.contains("client_auth")) {
            config.security.client_auth = require_bool(sec["client_auth"], "security.client_auth");
        }
        if (sec.contains("credential_name")) {
            config.security.credential_name = require_string(sec["credential_name"], "security.credential_name");
            if (!is_valid_credential_name(config.security.credential_name)) {
                throw GatewayException(400, "bad_request", "Invalid security.credential_name.");
            }
        }
        if (sec.contains("admin_credential_name")) {
            config.security.admin_credential_name = require_string(sec["admin_credential_name"], "security.admin_credential_name");
            if (!is_valid_credential_name(config.security.admin_credential_name)) {
                throw GatewayException(400, "bad_request", "Invalid security.admin_credential_name.");
            }
        }
        if (sec.contains("cors")) {
            const auto& cors = sec["cors"];
            static const std::set<std::string> cors_keys = {"allowed_origins", "enabled", "allowed_methods", "allowed_headers", "max_age_sec"};
            check_unknown_keys(cors, cors_keys, "security.cors");
            if (cors.contains("enabled")) config.security.cors_enabled = require_bool(cors["enabled"], "security.cors.enabled");
            if (cors.contains("allowed_methods")) config.security.allowed_methods = require_string_array(cors["allowed_methods"], "security.cors.allowed_methods");
            for (const auto& method : config.security.allowed_methods) {
                if (method != "GET" && method != "POST" && method != "PUT" && method != "PATCH" && method != "DELETE" && method != "OPTIONS") throw GatewayException(400, "bad_request", "Invalid CORS method.");
            }
            if (cors.contains("allowed_headers")) config.security.allowed_headers = require_string_array(cors["allowed_headers"], "security.cors.allowed_headers");
            const std::regex header_token("^[!#$%&'*+.^_`|~0-9A-Za-z-]+$");
            for (const auto& header : config.security.allowed_headers) {
                if (!std::regex_match(header, header_token)) throw GatewayException(400, "bad_request", "Invalid CORS header.");
            }
            if (cors.contains("max_age_sec")) config.security.cors_max_age_sec = static_cast<uint32_t>(integer_in_range(cors["max_age_sec"], 0, 31536000, "security.cors.max_age_sec"));
            if (cors.contains("allowed_origins")) {
                auto origins = require_string_array(cors["allowed_origins"], "security.cors.allowed_origins");
                for (const auto& origin : origins) {
                    if (!is_valid_origin(origin)) {
                        throw GatewayException(400, "bad_request", "Invalid CORS origin format: " + origin);
                    }
                    config.security.allowed_origins.push_back(origin);
                }
            }
        }
    }

    // 7. credentials
    if (j.contains("credentials")) {
        const auto& c = j["credentials"];
        static const std::set<std::string> cred_keys = {"service_name", "environment_fallback"};
        check_unknown_keys(c, cred_keys, "credentials");
        if (c.contains("service_name")) {
            config.credentials.service_name = require_string(c["service_name"], "credentials.service_name");
            if (!is_valid_credential_name(config.credentials.service_name)) {
                throw GatewayException(400, "bad_request", "Invalid credentials.service_name.");
            }
        }
        if (c.contains("environment_fallback")) {
            config.credentials.environment_fallback = require_bool(c["environment_fallback"], "credentials.environment_fallback");
        }
    }

    // 8. embedded_engine
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    int default_threads = (std::max)(1, static_cast<int>(hardware_threads / 2));

    auto parse_model_file = [&](const nlohmann::json& mf, const std::string& name) -> ModelFileConfig {
        static const std::set<std::string> mf_keys = {"path", "n_threads", "n_gpu_layers"};
        check_unknown_keys(mf, mf_keys, name);

        ModelFileConfig mfc;
        if (!mf.contains("path") || !mf["path"].is_string()) {
            throw GatewayException(400, "bad_request", name + ".path is required.");
        }
        mfc.path = mf["path"].get<std::string>();
        if (mfc.path.empty() || mfc.path.find('\0') != std::string::npos) {
            throw GatewayException(400, "bad_request", name + ".path must not be empty.");
        }

        // Check for UNC path
        if (mfc.path.rfind("\\\\", 0) == 0 || mfc.path.rfind("//", 0) == 0) {
            throw GatewayException(400, "bad_request", name + ".path UNC paths are rejected: " + mfc.path);
        }

        if (validate_model_files && config.embedded_engine.enabled) {
            std::filesystem::path fp = path_from_utf8(mfc.path);
            std::error_code ec;
            if (!std::filesystem::is_regular_file(fp, ec)) {
                throw GatewayException(400, "bad_request", name + ".path must be an existing regular file: " + mfc.path);
            }
        }

        if (mf.contains("n_threads")) {
            int64_t max_threads = hardware_threads > 0 ? static_cast<int64_t>(hardware_threads) : 1024;
            mfc.n_threads = static_cast<int>(integer_in_range(mf["n_threads"], 1, max_threads, name + ".n_threads"));
        } else {
            mfc.n_threads = default_threads;
        }

        if (mf.contains("n_gpu_layers")) {
            int64_t gl = integer_in_range(mf["n_gpu_layers"], 0, 0, name + ".n_gpu_layers");
            mfc.n_gpu_layers = static_cast<int>(gl);
        }
        return mfc;
    };

    if (j.contains("embedded_engine")) {
        const auto& ee = j["embedded_engine"];
        static const std::set<std::string> ee_keys = {"enabled", "embedding_model", "reranker_model"};
        check_unknown_keys(ee, ee_keys, "embedded_engine");
        if (ee.contains("enabled")) {
            config.embedded_engine.enabled = require_bool(ee["enabled"], "embedded_engine.enabled");
        }
        if (ee.contains("embedding_model")) {
            config.embedded_engine.embedding_model = parse_model_file(ee["embedding_model"], "embedded_engine.embedding_model");
        }
        if (ee.contains("reranker_model")) {
            config.embedded_engine.reranker_model = parse_model_file(ee["reranker_model"], "embedded_engine.reranker_model");
        }
    }

    // 9. providers
    std::set<std::string> provider_ids;
    if (j.contains("providers")) {
        const auto& provs = j["providers"];
        if (!provs.is_object()) {
            throw GatewayException(400, "bad_request", "providers must be an object.");
        }
        for (auto it = provs.begin(); it != provs.end(); ++it) {
            std::string pid = it.key();
            if (pid != "embedded" && !is_valid_provider_id(pid)) {
                throw GatewayException(400, "bad_request", "Invalid provider ID: '" + pid + "'");
            }
            if (provider_ids.find(pid) != provider_ids.end()) {
                throw GatewayException(400, "bad_request", "Duplicate provider ID: '" + pid + "'");
            }
            provider_ids.insert(pid);

            const auto& pobj = it.value();
            static const std::set<std::string> p_keys = {
                "base_url", "authentication", "key_vendor", "key_slot", "model_discovery", "allowlist"
            };
            check_unknown_keys(pobj, p_keys, "provider '" + pid + "'");

            ProviderConfig pc;
            pc.id = pid;

            if (pid == "embedded") {
                if (pobj.contains("base_url")) {
                    throw GatewayException(400, "bad_request", "embedded provider must not define base_url.");
                }
            } else {
                if (!pobj.contains("base_url") || !pobj["base_url"].is_string()) {
                    throw GatewayException(400, "bad_request", "External provider '" + pid + "' requires base_url.");
                }
                pc.base_url = pobj["base_url"].get<std::string>();
                if (!is_valid_url(*pc.base_url)) {
                    throw GatewayException(400, "bad_request", "Invalid base_url for provider '" + pid + "': " + *pc.base_url);
                }
            }

            if (pobj.contains("authentication")) {
                std::string auth = require_string(pobj["authentication"], "provider '" + pid + "' authentication");
                if (auth == "none") {
                    pc.authentication = Authentication::None;
                } else if (auth == "bearer") {
                    if (pid == "embedded") {
                        throw GatewayException(400, "bad_request", "embedded provider authentication must be none.");
                    }
                    pc.authentication = Authentication::Bearer;
                } else {
                    throw GatewayException(400, "bad_request", "Unknown authentication mode: " + auth);
                }
            }

            if (pc.authentication == Authentication::Bearer) {
                if (pobj.contains("key_vendor")) pc.key_vendor = require_string(pobj["key_vendor"], "provider '" + pid + "' key_vendor");
                if (pobj.contains("key_slot")) pc.key_slot = require_string(pobj["key_slot"], "provider '" + pid + "' key_slot");
                if (pc.key_vendor.empty() || pc.key_slot.empty()) {
                    throw GatewayException(400, "bad_request", "Bearer authentication requires key_vendor and key_slot.");
                }
            }

            if (pobj.contains("model_discovery")) {
                pc.model_discovery = require_bool(pobj["model_discovery"], "provider '" + pid + "' model_discovery");
            }

            if (pobj.contains("allowlist")) {
                auto patterns = require_string_array(pobj["allowlist"], "provider '" + pid + "' allowlist");
                for (const auto& pat : patterns) {
                    if (!is_valid_allowlist_pattern(pat)) {
                        throw GatewayException(400, "bad_request", "Invalid allowlist pattern: '" + pat + "' in provider '" + pid + "'");
                    }
                    pc.allowlist.push_back(pat);
                }
            }

            config.providers.push_back(std::move(pc));
        }
    }

    // 10. models (aliases)
    std::set<std::string> alias_names;
    if (j.contains("models")) {
        const auto& mdls = j["models"];
        if (!mdls.is_object()) {
            throw GatewayException(400, "bad_request", "models must be an object.");
        }
        for (auto it = mdls.begin(); it != mdls.end(); ++it) {
            std::string alias = it.key();
            if (!is_valid_alias_name(alias)) {
                throw GatewayException(400, "bad_request", "Invalid alias name: '" + alias + "'");
            }
            if (provider_ids.find(alias) != provider_ids.end()) {
                throw GatewayException(400, "bad_request", "Alias name clashes with provider ID: '" + alias + "'");
            }
            if (alias_names.find(alias) != alias_names.end()) {
                throw GatewayException(400, "bad_request", "Duplicate alias name: '" + alias + "'");
            }
            alias_names.insert(alias);

            const auto& mobj = it.value();
            static const std::set<std::string> m_keys = {"provider", "model", "input_prefix"};
            check_unknown_keys(mobj, m_keys, "model alias '" + alias + "'");

            ModelAlias ma;
            ma.alias = alias;
            if (!mobj.contains("provider") || !mobj["provider"].is_string()) {
                throw GatewayException(400, "bad_request", "Alias '" + alias + "' requires provider.");
            }
            ma.provider = mobj["provider"].get<std::string>();

            if (provider_ids.find(ma.provider) == provider_ids.end()) {
                throw GatewayException(400, "bad_request", "Alias '" + alias + "' references undefined provider '" + ma.provider + "'");
            }

            if (!mobj.contains("model") || !mobj["model"].is_string()) {
                throw GatewayException(400, "bad_request", "Alias '" + alias + "' requires model.");
            }
            ma.model = mobj["model"].get<std::string>();

            if (ma.provider == "embedded") {
                // Must have either embedding_model or reranker_model defined
                if (!config.embedded_engine.embedding_model && !config.embedded_engine.reranker_model) {
                    throw GatewayException(400, "bad_request", "Alias '" + alias + "' uses embedded provider but no embedded models are defined.");
                }
            }

            if (mobj.contains("input_prefix")) {
                ma.input_prefix = require_string(mobj["input_prefix"], "model alias '" + alias + "' input_prefix");
            }

            config.models.push_back(std::move(ma));
        }
    }

    return config;
}

GatewayConfig load_config_from_file(const std::filesystem::path& path, bool validate_model_files) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        throw GatewayException(400, "bad_request", "Configuration file not found: " + path_to_utf8(path));
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw GatewayException(400, "bad_request", "Failed to open configuration file: " + path_to_utf8(path));
    }
    nlohmann::json j;
    try {
        ifs >> j;
    } catch (const std::exception& e) {
        throw GatewayException(400, "bad_request", std::string("Malformed JSON in configuration file: ") + e.what());
    }
    return parse_and_validate_config(j, validate_model_files);
}

nlohmann::json serialize_config(const GatewayConfig& config) {
    nlohmann::json j;
    j["schema_version"] = config.schema_version;

    j["listen"] = {
        {"host", config.listen.host},
        {"port", config.listen.port}
    };

    j["limits"] = {
        {"max_request_bytes", config.limits.max_request_bytes}
    };

    j["timeouts"] = {
        {"upstream_connect_ms", config.timeouts.upstream_connect_ms},
        {"upstream_read_ms", config.timeouts.upstream_read_ms},
        {"upstream_write_ms", config.timeouts.upstream_write_ms}
    };
    j["model_cache"] = {{"enabled", config.model_cache.enabled}, {"ttl_sec", config.model_cache.ttl_sec}};

    j["logging"] = {
        {"level", config.logging.level},
        {"dir", config.logging.dir}
    };
    if (config.logging.file) j["logging"]["file"] = *config.logging.file;

    nlohmann::json sec = {
        {"client_auth", config.security.client_auth},
        {"credential_name", config.security.credential_name},
        {"admin_credential_name", config.security.admin_credential_name},
        {"cors", {
            {"allowed_origins", config.security.allowed_origins},
            {"enabled", config.security.cors_enabled},
            {"allowed_methods", config.security.allowed_methods},
            {"allowed_headers", config.security.allowed_headers},
            {"max_age_sec", config.security.cors_max_age_sec}
        }}
    };
    j["security"] = sec;

    j["credentials"] = {
        {"service_name", config.credentials.service_name},
        {"environment_fallback", config.credentials.environment_fallback}
    };

    unsigned int hw_threads = std::thread::hardware_concurrency();
    int default_threads = (std::max)(1, static_cast<int>(hw_threads / 2));

    nlohmann::json ee;
    ee["enabled"] = config.embedded_engine.enabled;
    if (config.embedded_engine.embedding_model) {
        int nt = config.embedded_engine.embedding_model->n_threads > 0
            ? config.embedded_engine.embedding_model->n_threads
            : default_threads;
        ee["embedding_model"] = {
            {"path", config.embedded_engine.embedding_model->path},
            {"n_threads", nt},
            {"n_gpu_layers", 0}
        };
    }
    if (config.embedded_engine.reranker_model) {
        int nt = config.embedded_engine.reranker_model->n_threads > 0
            ? config.embedded_engine.reranker_model->n_threads
            : default_threads;
        ee["reranker_model"] = {
            {"path", config.embedded_engine.reranker_model->path},
            {"n_threads", nt},
            {"n_gpu_layers", 0}
        };
    }
    j["embedded_engine"] = ee;

    nlohmann::json mdls = nlohmann::json::object();
    for (const auto& ma : config.models) {
        nlohmann::json m = {
            {"provider", ma.provider},
            {"model", ma.model}
        };
        if (ma.input_prefix) {
            m["input_prefix"] = *ma.input_prefix;
        }
        mdls[ma.alias] = m;
    }
    j["models"] = mdls;

    nlohmann::json provs = nlohmann::json::object();
    for (const auto& p : config.providers) {
        nlohmann::json pj;
        if (p.base_url) pj["base_url"] = *p.base_url;
        pj["authentication"] = (p.authentication == Authentication::Bearer) ? "bearer" : "none";
        if (p.authentication == Authentication::Bearer) {
            pj["key_vendor"] = p.key_vendor;
            pj["key_slot"] = p.key_slot;
        }
        pj["model_discovery"] = p.model_discovery;
        pj["allowlist"] = p.allowlist;
        provs[p.id] = pj;
    }
    j["providers"] = provs;

    return j;
}

} // namespace gateway
