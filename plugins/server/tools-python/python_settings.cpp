#include "python_settings.hpp"

namespace minimal::tools_python {
namespace {

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* key, T fallback) {
    auto key_it = parent.find(key);
    if (key_it == parent.end() || !key_it->is_number()) return fallback;
    return key_it->get<T>();
}

std::string StringOr(const nlohmann::json& parent, const char* key, std::string fallback) {
    auto key_it = parent.find(key);
    if (key_it == parent.end() || !key_it->is_string()) return fallback;
    return key_it->get<std::string>();
}

std::vector<std::string> StringListOr(const nlohmann::json& parent, const char* key) {
    std::vector<std::string> out;
    auto key_it = parent.find(key);
    if (key_it == parent.end() || !key_it->is_array()) return out;
    for (const auto& item : *key_it) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

}  // namespace

PythonSettings ParsePythonSettings(const nlohmann::json& config) {
    PythonSettings settings;
    if (!config.is_object()) return settings;

    settings.python = StringOr(config, "python", settings.python);
    // A string here would be silently dropped by ValueOr's is_number guard;
    // that is the documented shape (seconds), not worth a second codec.
    settings.sync_wait_ceiling = ValueOr(config, "sync_wait_ceiling", settings.sync_wait_ceiling);
    settings.startup_timeout = ValueOr(config, "startup_timeout", settings.startup_timeout);
    settings.install_timeout = ValueOr(config, "install_timeout", settings.install_timeout);
    settings.grace_seconds = ValueOr(config, "grace_seconds", settings.grace_seconds);
    settings.max_output_bytes = ValueOr(config, "max_output_bytes", settings.max_output_bytes);
    settings.max_capture_bytes = ValueOr(config, "max_capture_bytes", settings.max_capture_bytes);
    settings.rlimit_cpu_seconds = ValueOr(config, "rlimit_cpu_seconds", settings.rlimit_cpu_seconds);
    settings.max_file_size_bytes =
        ValueOr(config, "max_file_size_bytes", settings.max_file_size_bytes);
    settings.stream_head_bytes = ValueOr(config, "stream_head_bytes", settings.stream_head_bytes);
    settings.stream_tail_bytes = ValueOr(config, "stream_tail_bytes", settings.stream_tail_bytes);
    settings.background_max_runtime =
        ValueOr(config, "background_max_runtime", settings.background_max_runtime);

    if (settings.python.empty()) settings.python = "python3";
    if (settings.sync_wait_ceiling < 0) settings.sync_wait_ceiling = 0;
    if (settings.startup_timeout <= 0) settings.startup_timeout = 10;
    if (settings.install_timeout <= 0) settings.install_timeout = 300;
    if (settings.grace_seconds <= 0) settings.grace_seconds = 2;
    if (settings.max_output_bytes <= 0) settings.max_output_bytes = 1024 * 1024;
    if (settings.max_capture_bytes <= 0) settings.max_capture_bytes = 1024 * 1024;
    if (settings.rlimit_cpu_seconds < 0) settings.rlimit_cpu_seconds = 0;
    if (settings.max_file_size_bytes <= 0) settings.max_file_size_bytes = 1024LL * 1024 * 1024;
    if (settings.stream_head_bytes <= 0) settings.stream_head_bytes = 256 * 1024;
    if (settings.stream_tail_bytes <= 0) settings.stream_tail_bytes = 768 * 1024;
    if (settings.background_max_runtime < 0) settings.background_max_runtime = 0;
    return settings;
}

PythonSlice ParseSlice(const nlohmann::json* tool_config) {
    PythonSlice slice;
    if (tool_config == nullptr || !tool_config->is_object()) return slice;
    slice.packages = StringListOr(*tool_config, "packages");
    slice.requirements = StringListOr(*tool_config, "requirements");
    return slice;
}

}  // namespace minimal::tools_python
