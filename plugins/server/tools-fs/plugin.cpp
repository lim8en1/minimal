// tools-fs -- read / write / edit.
//
// A bundled plugin, using exactly the seams a third-party plugin uses. These
// tools are not structurally privileged; they are pre-installed and enabled by
// default because losing them leaves a useless harness, not a degraded one.
//
// Path safety is NOT reimplemented here: the tools declare
// path_confinement_supported() and ExtractPath(), and ToolExecutor does the
// out-of-workdir classification centrally, so that rule cannot drift per tool.

#include "minimal/plugins/plugin_abi.h"

#include "fs_settings.hpp"
#include "fs_tools.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-fs")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own `config` object from its plugins.json entry, verbatim;
    // core imposes no schema, this plugin owns the shape of its own slice.
    const auto settings = minimal::tools_fs::ParseFsSettings(context.config);

    registry.Register(minimal::tools_fs::MakeReadTool(settings));
    registry.Register(minimal::tools_fs::MakeWriteTool(settings));
    registry.Register(minimal::tools_fs::MakeEditTool(settings));
}

}  // extern "C"
