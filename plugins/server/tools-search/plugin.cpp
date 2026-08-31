// tools-search -- grep / glob.
//
// A bundled plugin, using exactly the seams a third-party plugin uses. These
// tools are not structurally privileged; they are pre-installed and enabled by
// default because searching a tree through `bash` round-trips results through
// the context window with no structure and no ignore rules -- losing a
// dedicated search tool degrades the one thing this harness exists to be,
// token efficiency.
//
// Path safety is NOT reimplemented here: both tools declare
// path_confinement_supported() and ExtractPath(), and ToolExecutor does the
// out-of-workdir classification centrally, so that rule cannot drift per tool.
//
// Regex matching is RE2, and that is load-bearing: patterns are
// model-authored, and a backtracking engine would let a pathological pattern
// wedge the session worker thread -- the hang bash's timeout_ms exists to
// bound. Linear time makes that failure mode unreachable instead of bounded.

#include "minimal/plugins/plugin_abi.h"

#include "glob_tool.hpp"
#include "grep_tool.hpp"
#include "search_settings.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-search"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own `config` object from its plugins.json entry, verbatim;
    // core imposes no schema, this plugin owns the shape of its own slice.
    const auto settings = minimal::tools_search::ParseSearchSettings(context.config);

    registry.Register(minimal::tools_search::MakeGrepTool(settings));
    registry.Register(minimal::tools_search::MakeGlobTool(settings));
}

}  // extern "C"
