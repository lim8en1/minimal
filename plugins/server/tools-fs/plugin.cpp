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

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-fs"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::ToolRegistrationContext& context) {
    // Core hands over the whole `tools` object without imposing a schema; this
    // plugin owns the shape of its own slice.
    const auto settings = minimal::tools_fs::ParseFsSettings(context.tools_settings);

    registry.Register(minimal::tools_fs::MakeReadTool(settings));
    registry.Register(minimal::tools_fs::MakeWriteTool(settings));
    registry.Register(minimal::tools_fs::MakeEditTool(settings));
}

}  // extern "C"
