#include "minimal/client/plugins/plugin_discovery.hpp"

namespace minimal::client::plugins {
namespace {

common::PluginConfigFormat Format() {
    common::PluginConfigFormat format;
    format.file_label = "client-plugins.json";
    format.noun = "client plugin";
    format.config_dir_label = common::kClientPluginConfigDirName;
    return format;
}

}  // namespace

ClientPluginConfig ParseClientPluginConfig(const std::string& json_text) {
    return common::ParsePluginConfigFile(json_text, Format());
}

std::string SerializeClientPluginConfig(const ClientPluginConfig& config) {
    return common::SerializePluginConfigFile(config);
}

std::vector<PluginCandidate> DiscoverClientPlugins(const std::filesystem::path& bundled_dir,
                                                   const std::filesystem::path& user_dir,
                                                   ClientPluginConfig& config,
                                                   const std::filesystem::path& plugin_config_dir) {
    common::PluginConfigFormat format = Format();
    format.config_dir = plugin_config_dir;
    return common::DiscoverPluginFiles(bundled_dir, user_dir, config, format);
}

}  // namespace minimal::client::plugins
