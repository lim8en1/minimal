#include "bash_settings.hpp"

namespace minimal::tools_bash {
namespace {

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    auto key_it = section_it->find(key);
    if (key_it == section_it->end() || !key_it->is_number()) return fallback;
    return key_it->get<T>();
}

}  // namespace

BashSettings ParseBashSettings(const nlohmann::json& tools_settings) {
    BashSettings settings;
    if (!tools_settings.is_object()) return settings;

    settings.timeout_ms = ValueOr(tools_settings, "bash", "timeout_ms", settings.timeout_ms);
    settings.max_timeout_ms =
        ValueOr(tools_settings, "bash", "max_timeout_ms", settings.max_timeout_ms);
    settings.max_output_bytes =
        ValueOr(tools_settings, "bash", "max_output_bytes", settings.max_output_bytes);
    settings.max_file_size_bytes =
        ValueOr(tools_settings, "bash", "max_file_size_bytes", settings.max_file_size_bytes);

    auto bash_it = tools_settings.find("bash");
    if (bash_it != tools_settings.end() && bash_it->is_object()) {
        auto shell_it = bash_it->find("shell");
        if (shell_it != bash_it->end() && shell_it->is_string()) {
            settings.shell = shell_it->get<std::string>();
        }
    }

    // A non-positive timeout would kill every command instantly; a non-positive
    // output cap would persist nothing. Neither is ever what was meant.
    if (settings.timeout_ms <= 0) settings.timeout_ms = 120000;
    if (settings.max_output_bytes <= 0) settings.max_output_bytes = 1024 * 1024;
    if (settings.max_file_size_bytes <= 0) settings.max_file_size_bytes = 1024LL * 1024 * 1024;
    if (settings.shell.empty()) settings.shell = "/bin/sh";
    // A ceiling below the default would silently shorten every command that did
    // not ask for a timeout, which is not what configuring a *maximum* means.
    if (settings.max_timeout_ms < settings.timeout_ms) settings.max_timeout_ms = settings.timeout_ms;
    return settings;
}

}  // namespace minimal::tools_bash
