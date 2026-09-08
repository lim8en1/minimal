#include "windbg_settings.hpp"

namespace minimal::tools_windbg {

namespace {

std::string StringOr(const nlohmann::json& json, const char* key, std::string fallback) {
    if (json.contains(key) && json.at(key).is_string()) return json.at(key).get<std::string>();
    return fallback;
}

std::int64_t IntOr(const nlohmann::json& json, const char* key, std::int64_t fallback) {
    if (json.contains(key) && json.at(key).is_number_integer()) {
        return json.at(key).get<std::int64_t>();
    }
    return fallback;
}

}  // namespace

WindbgSettings ParseWindbgSettings(const nlohmann::json& config) {
    WindbgSettings settings;
    if (!config.is_object()) return settings;

    settings.base_url = StringOr(config, "base_url", settings.base_url);
    // Trailing slashes would double up in path joins; normalize once.
    while (settings.base_url.size() > 1 && settings.base_url.back() == '/') {
        settings.base_url.pop_back();
    }
    settings.bearer_token = StringOr(config, "bearer_token", settings.bearer_token);
    settings.backend = StringOr(config, "backend", settings.backend);
    if (config.contains("protocol_version") && config.at("protocol_version").is_number_integer()) {
        settings.protocol_version = config.at("protocol_version").get<int>();
    }
    settings.exec_grace_s = IntOr(config, "exec_grace_s", settings.exec_grace_s);
    settings.request_timeout_ms = IntOr(config, "request_timeout_ms", settings.request_timeout_ms);
    settings.job_wait_s = IntOr(config, "job_wait_s", settings.job_wait_s);
    settings.sync_wait_ceiling = IntOr(config, "sync_wait_ceiling", settings.sync_wait_ceiling);
    settings.max_output_bytes = IntOr(config, "max_output_bytes", settings.max_output_bytes);
    if (config.contains("max_jobs") && config.at("max_jobs").is_number_integer()) {
        settings.max_jobs = config.at("max_jobs").get<int>();
    }
    if (config.contains("default_target") && config.at("default_target").is_object()) {
        settings.default_target = config.at("default_target");
    }
    settings.on_session_end = StringOr(config, "on_session_end", settings.on_session_end);
    return settings;
}

}  // namespace minimal::tools_windbg
