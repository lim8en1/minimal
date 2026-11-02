#include "python_settings.hpp"

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::tools_python {

PythonSettings ParsePythonSettings(const nlohmann::json& config) {
    PythonSettings settings;
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "tools-python"));
    reader.Describe("python", {"Fallback interpreter (bare python and venv creation), "
                               "resolved on PATH."});
    // Live: what python_exec reads per call. Restart: what an interpreter
    // copies at spawn (a change would reach only interpreters started later).
    reader.Describe("sync_wait_ceiling_s", {"Ceiling on a model-supplied sync timeout; reaching "
                                            "it demotes the wait to async. 0 = unlimited.",
                                            config::ApplyPolicy::kLive});
    reader.Describe("startup_timeout_s", {"Spawn + handshake window for a new interpreter."});
    reader.Describe("install_timeout_s", {"Bound on venv creation / package installation."});
    reader.Describe("grace_s", {"How long an interrupt waits before escalating to a kill."});
    reader.Describe("background_max_runtime_s",
                    {"Default deadline for background requests. 0 = off.",
                     config::ApplyPolicy::kLive});
    reader.Describe("rlimit_cpu_s", {"RLIMIT_CPU backstop for the interpreter. 0 = off."});
    reader.Describe("max_output_bytes", {"Bounds the persisted result text per request."});
    reader.Describe("max_capture_bytes", {"Per-field capture cap (stdout, stderr, result, ...)."});
    reader.Describe("max_file_size_bytes", {"RLIMIT_FSIZE backstop for files the code writes."});
    reader.Describe("memory_max_percent",
                    {"Interpreter memory ceiling, percent of RAM (Linux systemd scope); 0 = none."});
    reader.Describe("stream_head_bytes", {"Live-output ring: the first bytes kept."});
    reader.Describe("stream_tail_bytes", {"Live-output ring: the last bytes kept."});
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
    settings.memory_max_percent = static_cast<int>(
        reader.Int("memory_max_percent", settings.memory_max_percent, 0, 100));
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
