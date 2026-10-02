#include "minimal/plugins/plugin_discovery.hpp"

#include <string_view>

#include "minimal/common/bundled_assets.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::plugins {
namespace {

// Bundled plugins that default to DISABLED despite their provenance. Everything
// else bundled is load-bearing for the default harness (tools, provider,
// commands) and ships enabled. The list exists because session-titler is the
// first bundled plugin whose entire job is to spend the user's tokens on its
// own initiative (~100 per session) -- trusted code, but a cost the operator
// should opt into. Match by basename, the same key plugins.json entries use.
bool DefaultsDisabled(std::string_view file) {
    // session-titler: spends the user's tokens on its own initiative.
    // (memory is NOT here: unconfigured it makes no network calls and spawns
    // nothing, its writes go through the file_write permission policy, and a
    // store that cannot open degrades the plugin instead of failing startup.)
    // typesafe-advisor: sends tool input to an external endpoint AND auto-answers
    // permission prompts on the model's behalf -- the operator's two informed
    // decisions, not defaults. All ship present but disabled; once an entry
    // exists in plugins.json the user's `enabled` governs and this list is never
    // consulted again.
    return file == "session-titler.so" || file == "session-titler.dylib" ||
           file == "typesafe-advisor.so" || file == "typesafe-advisor.dylib";
}

common::PluginConfigFormat Format() {
    common::PluginConfigFormat format;
    format.file_label = "plugins.json";
    format.noun = "plugin";
    format.defaults_disabled = DefaultsDisabled;
    // No legacy fallback: the old key is IGNORED (it round-trips via `preserved`,
    // so it is not deleted either). Silence would mean aliases quietly not
    // applying after an upgrade, so say so on every boot -- the noise is the
    // feature.
    format.check_keys = [](const nlohmann::json& parsed) {
        if (parsed.contains("command_aliases")) {
            common::DefaultLogger().warn(
                "plugins.json: \"command_aliases\" was renamed to \"aliases\" and no longer "
                "applies; rename the key");
        }
    };
    return format;
}

}  // namespace

PluginConfig ParsePluginConfig(const std::string& json_text) {
    return common::ParsePluginConfigFile(json_text, Format());
}

std::string SerializePluginConfig(const PluginConfig& config) {
    return common::SerializePluginConfigFile(config);
}

// Kept in this namespace because that is where callers look for it; the layout
// probe itself is shared with the bundled prompt dir in minimal::common.
std::filesystem::path BundledPluginDir() { return common::BundledPluginDir(); }

std::vector<PluginCandidate> DiscoverPlugins(const std::filesystem::path& bundled_dir,
                                             const std::filesystem::path& user_dir,
                                             PluginConfig& config) {
    return common::DiscoverPluginFiles(bundled_dir, user_dir, config, Format());
}

}  // namespace minimal::plugins
