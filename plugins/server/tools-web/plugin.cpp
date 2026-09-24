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
// is their gate. An operator who allows Execute for a sandboxed bash does
// not thereby allow fetching arbitrary URLs, and allowing the web tools
// (`network: allow`) does not require allowing bash.
//
// Result size: nothing tool-local is needed beyond HttpClient's own 8 MiB
// buffered-response cap -- the agent loop caps every tool's persisted
// result at one central chokepoint (agent_loop.cpp, "cap tool RESULTS"),
// regardless of which tool produced it.

#include "minimal/plugins/plugin_abi.h"

#include "fetch_tool.hpp"
#include "search_tool.hpp"
#include "web_settings.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-web")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    const auto settings = minimal::tools_web::ParseWebSettings(context.config);

    registry.Register(minimal::tools_web::MakeWebFetchTool(settings, minimal::net::HttpClient{}));
    registry.Register(minimal::tools_web::MakeWebSearchTool(settings, minimal::net::HttpClient{}));
}

}  // extern "C"
