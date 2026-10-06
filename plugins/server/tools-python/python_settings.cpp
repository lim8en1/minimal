#include "python_settings.hpp"

#include "minimal/common/config_reader.hpp"

namespace minimal::tools_python {

PythonSettings ParsePythonSettings(const nlohmann::json& config) {
    PythonSettings settings;
    config::Reader reader(config, "plugins.json", "tools-python.config");
    settings.python = reader.String("python", settings.python, /*allow_empty=*/false);
    // Every duration is in SECONDS and says so in its key.
    settings.sync_wait_ceiling =
        reader.Int("sync_wait_ceiling_s", settings.sync_wait_ceiling, 0, kMaxSaneRuntimeSeconds);
    settings.startup_timeout =
        reader.Int("startup_timeout_s", settings.startup_timeout, 1, kMaxSaneRuntimeSeconds);
    settings.install_timeout =
        reader.Int("install_timeout_s", settings.install_timeout, 1, kMaxSaneRuntimeSeconds);
    settings.grace_seconds = reader.Int("grace_s", settings.grace_seconds, 1, kMaxSaneRuntimeSeconds);
    settings.background_max_runtime = reader.Int(
        "background_max_runtime_s", settings.background_max_runtime, 0, kMaxSaneRuntimeSeconds);
    settings.rlimit_cpu_seconds =
        reader.Int("rlimit_cpu_s", settings.rlimit_cpu_seconds, 0, kMaxSaneRuntimeSeconds);
    settings.max_output_bytes = reader.Int("max_output_bytes", settings.max_output_bytes, 1);
    settings.max_capture_bytes = reader.Int("max_capture_bytes", settings.max_capture_bytes, 1);
    settings.max_file_size_bytes =
        reader.Int("max_file_size_bytes", settings.max_file_size_bytes, 1);
    settings.stream_head_bytes = reader.Int("stream_head_bytes", settings.stream_head_bytes, 1);
    settings.stream_tail_bytes = reader.Int("stream_tail_bytes", settings.stream_tail_bytes, 1);
    reader.Finish();
    return settings;
}

PythonSlice ParseSlice(const nlohmann::json* tool_config) {
    PythonSlice slice;
    if (tool_config == nullptr || tool_config->is_null()) return slice;
    // The preset's `tool_config.python` slice: strict like the plugin config,
    // so a typo'd `package:` is named instead of declaring nothing.
    config::Reader reader(*tool_config, "agent preset", "tool_config.python");
    slice.packages = reader.StringList("packages");
    slice.requirements = reader.StringList("requirements");
    reader.Finish();
    return slice;
}

}  // namespace minimal::tools_python
