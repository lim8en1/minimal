#include "bash_settings.hpp"

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::tools_bash {

BashSettings ParseBashSettings(const nlohmann::json& config) {
    BashSettings settings;
    // The plugin's own plugins.json "config" object -- flat, because every
    // knob belongs to this one tool. Timeouts are configured in SECONDS (the
    // unit the model-facing tool input uses) and stored in ms.
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "tools-bash"));
    constexpr std::int64_t kMaxSeconds = 7LL * 24 * 3600;
    settings.timeout_ms = reader.Int("timeout_s", settings.timeout_ms / 1000, 1, kMaxSeconds) * 1000;
    settings.max_timeout_ms =
        reader.Int("max_timeout_s", settings.max_timeout_ms / 1000, 1, kMaxSeconds) * 1000;
    settings.background_max_timeout_ms =
        reader.Int("background_max_timeout_s", settings.background_max_timeout_ms / 1000, 1,
                   kMaxSeconds) *
        1000;
    settings.max_output_bytes = reader.Int("max_output_bytes", settings.max_output_bytes, 1);
    settings.max_file_size_bytes =
        reader.Int("max_file_size_bytes", settings.max_file_size_bytes, 1);
    settings.max_background_tasks = static_cast<int>(
        reader.Int("max_background_tasks", settings.max_background_tasks, 1, 1024));
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
