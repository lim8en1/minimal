#include "bash_settings.hpp"

#include <stdexcept>

namespace minimal::tools_bash {
namespace {

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* key, T fallback) {
    auto key_it = parent.find(key);
    if (key_it == parent.end() || !key_it->is_number()) return fallback;
    return key_it->get<T>();
}

}  // namespace

BashSettings ParseBashSettings(const nlohmann::json& config) {
    BashSettings settings;
    if (!config.is_object()) return settings;

    // The plugin's own plugins.json "config" object, verbatim -- keys are flat
    // because every knob in this schema belongs to this one tool.
    settings.timeout_ms = ValueOr(config, "timeout_ms", settings.timeout_ms);
    settings.max_timeout_ms = ValueOr(config, "max_timeout_ms", settings.max_timeout_ms);
    settings.background_max_timeout_ms =
        ValueOr(config, "background_max_timeout_ms", settings.background_max_timeout_ms);
    settings.max_output_bytes = ValueOr(config, "max_output_bytes", settings.max_output_bytes);
    settings.max_file_size_bytes =
        ValueOr(config, "max_file_size_bytes", settings.max_file_size_bytes);
    settings.max_background_tasks = ValueOr(config, "max_background_tasks",
                                            settings.max_background_tasks);

    auto shell_it = config.find("shell");
    if (shell_it != config.end() && shell_it->is_string()) {
        settings.shell = shell_it->get<std::string>();
    }

    auto env_it = config.find("inherit_env");
    if (env_it != config.end() && env_it->is_string()) {
        settings.inherit_env = env_it->get<std::string>();
        // A typo'd mode must not silently mean "allowlist" (or worse, "all"):
        // the two choices are named, everything else is a named parse error.
        if (settings.inherit_env != "allowlist" && settings.inherit_env != "all") {
            throw std::runtime_error(
                "tools-bash config: inherit_env must be \"allowlist\" or \"all\"");
        }
    }

    // A non-positive timeout would kill every command instantly; a non-positive
    // output cap would persist nothing. Neither is ever what was meant.
    if (settings.timeout_ms <= 0) settings.timeout_ms = 120000;
    if (settings.max_output_bytes <= 0) settings.max_output_bytes = 1024 * 1024;
    if (settings.max_file_size_bytes <= 0) settings.max_file_size_bytes = 1024LL * 1024 * 1024;
    if (settings.background_max_timeout_ms <= 0) settings.background_max_timeout_ms = 600000;
    if (settings.max_background_tasks <= 0) settings.max_background_tasks = 4;
    if (settings.shell.empty()) settings.shell = "/bin/sh";
    // A ceiling below the default would silently shorten every command that did
    // not ask for a timeout, which is not what configuring a *maximum* means.
    if (settings.max_timeout_ms < settings.timeout_ms) settings.max_timeout_ms = settings.timeout_ms;
    // Same rule for the background ceiling, against its own default.
    if (settings.background_max_timeout_ms < settings.timeout_ms) {
        settings.background_max_timeout_ms = settings.timeout_ms;
    }
    return settings;
}

}  // namespace minimal::tools_bash
