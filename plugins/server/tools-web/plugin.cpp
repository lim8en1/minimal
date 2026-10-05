// tools-web -- web_fetch and web_search.
//
// One plugin, two tools: they share the HTTP client, the settings parsing,
// the URL policy, and (for web_fetch) the truncation path -- splitting them
// would duplicate all of it for a genuinely small feature. web_search is
// inert and self-describing when its backend is unconfigured (its own
// error message names the config key), which is what a separate optional
// plugin would have bought anyway.
//
// Both tools are ReadOnly and declare network_access_required(): the
// `network` dimension (permission.hpp, ARCHITECTURE.md's Permission model)
// is their gate -- refined per host by this plugin's own permission model
// (host_permission_model.hpp), which falls back to core's scalar. An operator who allows Execute for a sandboxed bash does
// not thereby allow fetching arbitrary URLs, and allowing the web tools
// (`network: allow`) does not require allowing bash.
//
// Result size: nothing tool-local is needed beyond HttpClient's own 8 MiB
// buffered-response cap -- the agent loop caps every tool's persisted
// result at one central chokepoint (agent_loop.cpp, "cap tool RESULTS"),
// regardless of which tool produced it.

#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/service_registry.hpp"

#include "fetch_tool.hpp"
#include "host_permission_model.hpp"
#include "search_tool.hpp"
#include "web_settings.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-web")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // core.secrets is core-provided before any plugin loads.
    const auto* secrets =
        context.services != nullptr
            ? static_cast<const minimal::config::SecretResolver*>(
                  context.services->Find(minimal::config::kSecretsServiceName))
            : nullptr;
    const auto settings = minimal::tools_web::ParseWebSettings(context.config, secrets);

    registry.Register(minimal::tools_web::MakeWebFetchTool(settings, minimal::net::HttpClient{}));
    registry.Register(minimal::tools_web::MakeWebSearchTool(settings, minimal::net::HttpClient{}));
}

// Per-host rules for this plugin's own tools (host_permission_model.hpp):
// the network dimension, finer than core's one scalar.
void minimal_plugin_register_permission_models(
    minimal::tools::PermissionModelRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(minimal::tools_web::MakeHostPermissionModel(
        minimal::tools_web::ParseWebSettings(context.config)));
}

}  // extern "C"
