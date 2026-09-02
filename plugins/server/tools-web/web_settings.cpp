#include "web_settings.hpp"

#include <cstdlib>
#include <stdexcept>

namespace minimal::tools_web {
namespace {

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    auto key_it = section_it->find(key);
    if (key_it == section_it->end() || !key_it->is_number()) return fallback;
    return key_it->get<T>();
}

template <typename T>
T PositiveOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    const auto value = ValueOr(parent, section, key, fallback);
    return value > 0 ? value : fallback;
}

std::string StringOr(const nlohmann::json& parent, const char* section, const char* key,
                     std::string fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    auto key_it = section_it->find(key);
    if (key_it == section_it->end() || !key_it->is_string()) return fallback;
    return key_it->get<std::string>();
}

bool BoolOr(const nlohmann::json& parent, const char* section, const char* key, bool fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    auto key_it = section_it->find(key);
    if (key_it == section_it->end() || !key_it->is_boolean()) return fallback;
    return key_it->get<bool>();
}

}  // namespace

WebSettings ParseWebSettings(const nlohmann::json& config) {
    WebSettings settings;
    if (!config.is_object()) return settings;

    settings.fetch_timeout_ms = PositiveOr(config, "fetch", "timeout_ms", settings.fetch_timeout_ms);
    settings.fetch_max_redirects =
        PositiveOr(config, "fetch", "max_redirects", settings.fetch_max_redirects);
    settings.fetch_allow_private_hosts =
        BoolOr(config, "fetch", "allow_private_hosts", settings.fetch_allow_private_hosts);

    settings.search_api_url = StringOr(config, "search", "api_url", settings.search_api_url);
    settings.search_api_key_env =
        StringOr(config, "search", "api_key_env", settings.search_api_key_env);
    settings.search_default_max_results =
        PositiveOr(config, "search", "default_max_results", settings.search_default_max_results);
    settings.search_max_results =
        PositiveOr(config, "search", "max_results", settings.search_max_results);
    settings.search_timeout_ms =
        PositiveOr(config, "search", "timeout_ms", settings.search_timeout_ms);

    if (!settings.search_api_key_env.empty()) {
        const char* value = std::getenv(settings.search_api_key_env.c_str());
        if (value == nullptr || *value == '\0') {
            throw std::runtime_error("tools-web: search.api_key_env names '" +
                                     settings.search_api_key_env + "' which is unset");
        }
        settings.search_api_key = value;
    }

    return settings;
}

}  // namespace minimal::tools_web
