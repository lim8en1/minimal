#include "bash_settings.hpp"

#include <limits>

#include "minimal/common/clock.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::tools_bash {

BashSettings ParseBashSettings(const nlohmann::json& config) {
    BashSettings settings;
    // The plugin's own plugins.d/tools-bash.json -- flat, because every knob
    // belongs to this one tool. Timeouts are configured in SECONDS (the unit
    // the model-facing tool input uses) and stored in ms. The limits are
    // live: read per call, and never quoted in the tool's definition. The
    // shell and inherit_env seed per-session state held for the process.
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "tools-bash"));
    constexpr auto kLive = config::ApplyPolicy::kLive;
    reader.Describe("timeout_s", {"Default wall-clock cap per command, seconds; the whole "
                                  "process group is killed at the deadline.",
                                  kLive});
    reader.Describe("max_timeout_s", {"Ceiling on a model-supplied timeout, seconds.", kLive});
    reader.Describe("background_max_timeout_s",
                    {"Ceiling for detached (background) commands, seconds.", kLive});
    reader.Describe("max_background_tasks", {"Per-session cap on live background tasks.", kLive});
    reader.Describe("max_output_bytes", {"Bounds the persisted result the model sees.", kLive});
    reader.Describe("max_file_size_bytes",
                    {"RLIMIT_FSIZE backstop for files a command writes.", kLive});
    reader.Describe("shell", {"The POSIX shell (or a wrapper entering a container)."});
    reader.Describe("inherit_env", {"How the session environment is seeded: \"allowlist\" "
                                    "($HOME, $PATH, ...) or \"all\" (includes API keys)."});
    // The largest timeout a deadline can represent (common::kMaxDeadline).
    const std::int64_t kMaxSeconds = common::MillisToSeconds(common::kMaxDeadline.count());
    const auto seconds_key = [&reader](const char* key, std::int64_t default_ms) {
        return common::SecondsToMillis(
            reader.Int(key, common::MillisToSeconds(default_ms), 1, kMaxSeconds));
    };
    settings.timeout_ms = seconds_key("timeout_s", settings.timeout_ms);
    settings.max_timeout_ms = seconds_key("max_timeout_s", settings.max_timeout_ms);
    settings.background_max_timeout_ms =
        seconds_key("background_max_timeout_s", settings.background_max_timeout_ms);
    settings.max_output_bytes = reader.Int("max_output_bytes", settings.max_output_bytes, 1);
    settings.max_file_size_bytes =
        reader.Int("max_file_size_bytes", settings.max_file_size_bytes, 1);
    settings.max_background_tasks = static_cast<int>(
        reader.Int("max_background_tasks", settings.max_background_tasks, 1,
                   std::numeric_limits<int>::max()));
    settings.shell = reader.String("shell", settings.shell, /*allow_empty=*/false);
    // A typo'd mode must not silently mean "allowlist" (or worse, "all").
    settings.inherit_env = reader.Enum("inherit_env", settings.inherit_env, {"allowlist", "all"});
    reader.Finish();

    // A ceiling below the default would cut every command that did not ask for
    // a timeout -- the configuration contradicting itself.
    if (settings.max_timeout_ms < settings.timeout_ms) {
        reader.Fail("max_timeout_s", "must not be below timeout_s");
    }
    if (settings.background_max_timeout_ms < settings.timeout_ms) {
        reader.Fail("background_max_timeout_s", "must not be below timeout_s");
    }
    return settings;
}

}  // namespace minimal::tools_bash
