#include "minimal/plugins/plugin_discovery.hpp"

#include "minimal/common/bundled_assets.hpp"

namespace minimal::plugins {
namespace {

common::PluginConfigFormat Format() {
    common::PluginConfigFormat format;
    format.file_label = "plugins.json";
    format.noun = "plugin";
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
                                             PluginConfig& config,
                                             const std::filesystem::path& plugin_config_dir) {
    common::PluginConfigFormat format = Format();
    format.config_dir = plugin_config_dir;
    return common::DiscoverPluginFiles(bundled_dir, user_dir, config, format);
}

}  // namespace minimal::plugins
