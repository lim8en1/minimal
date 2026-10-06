// project-context -- the project's own instructions file (AGENTS.md, else
// CLAUDE.md) in every session's context, first: a context contributor at
// priority 0, read once per session from its project_root (creation, resume,
// subagent spawn alike) and frozen with the session's context tail.
//
// Config (plugins.json `config`, every key optional):
//   {"files": ["AGENTS.md", "CLAUDE.md"], "max_bytes": 262144, "search_ancestors": false}
// Disabled, no instructions file is read.

#include "minimal/agent/context_contributor.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "project_context.hpp"


MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "project-context"),
    [](const nlohmann::json& config) {
        (void)minimal::project_context::ParseProjectContextSettings(config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("project-context")

void minimal_plugin_register_context_contributors(
    minimal::agent::ContextContributorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(minimal::project_context::MakeProjectContextContributor(
        minimal::project_context::ParseProjectContextSettings(context.config)));
}

}  // extern "C"
