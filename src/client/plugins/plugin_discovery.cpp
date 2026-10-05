#include "minimal/client/plugins/plugin_discovery.hpp"

namespace minimal::client::plugins {
namespace {

common::PluginConfigFormat Format() {
    common::PluginConfigFormat format;
    format.file_label = "client-plugins.json";
    format.noun = "client plugin";
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
                                                   ClientPluginConfig& config) {
    return common::DiscoverPluginFiles(bundled_dir, user_dir, config, Format());
}

}  // namespace minimal::client::plugins
