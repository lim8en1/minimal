// tools-fs -- read / write / edit.
//
// A bundled plugin, using exactly the seams a third-party plugin uses. These
// tools are not structurally privileged; they are pre-installed and enabled by
// default because losing them leaves a useless harness, not a degraded one.
//
// Path safety is NOT reimplemented here: the tools declare
// path_confinement_supported() and ExtractPath(), and ToolExecutor does the
// out-of-workdir classification centrally, so that rule cannot drift per tool.

#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "fs_settings.hpp"
#include "fs_tools.hpp"

namespace {

// What read, write and edit read per call; swapped by minimal_plugin_reconfigure.
std::shared_ptr<minimal::common::LiveValue<minimal::tools_fs::FsSettings>>& Live() {
    static std::shared_ptr<minimal::common::LiveValue<minimal::tools_fs::FsSettings>> live;
    return live;
}

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "tools-fs"),
    [](const nlohmann::json& config) {
        (void)minimal::tools_fs::ParseFsSettings(config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-fs")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own plugins.d/tools-fs.json, verbatim; core imposes no
    // schema, this plugin owns the shape of its own file.
    Live() = std::make_shared<minimal::common::LiveValue<minimal::tools_fs::FsSettings>>(
        minimal::tools_fs::ParseFsSettings(context.config));

    registry.Register(minimal::tools_fs::MakeReadTool(Live()));
    registry.Register(minimal::tools_fs::MakeWriteTool(Live()));
    registry.Register(minimal::tools_fs::MakeEditTool(Live()));
}

// Every field is live: the three tools read the limits per call and their
// definitions quote none of them.
bool minimal_plugin_reconfigure(const nlohmann::json& config, std::string& error) {
    if (!Live()) {
        error = "tools-fs is not registered";
        return false;
    }
    try {
        Live()->Set(minimal::tools_fs::ParseFsSettings(config));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // extern "C"
