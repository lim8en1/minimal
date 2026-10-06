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

#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "glob_tool.hpp"
#include "grep_tool.hpp"
#include "search_settings.hpp"

namespace {

// What grep and glob read per call; swapped by minimal_plugin_reconfigure.
std::shared_ptr<minimal::common::LiveValue<minimal::tools_search::SearchSettings>>& Live() {
    static std::shared_ptr<minimal::common::LiveValue<minimal::tools_search::SearchSettings>> live;
    return live;
}

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "tools-search"),
    [](const nlohmann::json& config) {
        (void)minimal::tools_search::ParseSearchSettings(config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-search")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own plugins.d/tools-search.json, verbatim; core imposes no
    // schema, this plugin owns the shape of its own file.
    Live() = std::make_shared<minimal::common::LiveValue<minimal::tools_search::SearchSettings>>(
        minimal::tools_search::ParseSearchSettings(context.config));

    registry.Register(minimal::tools_search::MakeGrepTool(Live()));
    registry.Register(minimal::tools_search::MakeGlobTool(Live()));
}

// Every field is live: both tools read the limits per call and their
// definitions quote none of them.
bool minimal_plugin_reconfigure(const nlohmann::json& config, std::string& error) {
    if (!Live()) {
        error = "tools-search is not registered";
        return false;
    }
    try {
        Live()->Set(minimal::tools_search::ParseSearchSettings(config));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // extern "C"
