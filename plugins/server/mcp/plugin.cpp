// mcp -- the MCP client, as a bundled plugin.
//
// Everything MCP lives here; core knows none of it. The plugin reaches core
// only through generic seams:
//
//   register_tools              reserves the "mcp__" tool namespace and builds
//                               the manager (mcps.json + channels.json from the
//                               config dir); servers publish their tools as
//                               groups ("mcp__<server>__") when they come up
//   register_tool_sources       the servers as tool source "mcp": presets
//                               allow them under tool_sources.mcp, sessions
//                               activate them
//   register_permission_models  the per-server permission model (slices
//                               plugins.mcp / plugin_permissions.mcp)
//   register_commands           /mcp
//   register_services           "mcp.endpoint" for the mcp-view client plugin
//   core.signals (consumed)     channels.json channels as external signal
//                               channels: registered at startup, their
//                               notifications delivered through the router
//   shutdown                    stops every server process and the reaper
//
// Configuration problems (a bad entry, a command not on PATH, a channel name
// collision) are reported through report_warning and printed by the startup
// sanity check; they never fail registration. An invalid limit in this
// plugin's own config does -- fatal, as a bundled plugin.

#include "minimal/common/plugin_config_label.hpp"
#include "minimal/common/plugin_config_schema.hpp"
#include "minimal/plugins/plugin_abi.h"

#include <memory>

#include "mcp_channels.hpp"
#include "mcp_command.hpp"
#include "mcp_plugin_endpoint.hpp"
#include "mcp_settings.hpp"
#include "mcp_validation.hpp"
#include "minimal/agent/signal_service.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"
#include "minimal/mcp/mcp_permission_model.hpp"
#include "minimal/mcp/mcp_tool_source.hpp"
#include "minimal/mcp/naming.hpp"

namespace {

using minimal::plugins::PluginRegistrationContext;

// Process-lifetime state (plugins are never unloaded), built in
// register_tools -- the one hook with the ToolRegistry the manager publishes
// into -- and torn down in minimal_plugin_shutdown.
struct State {
    std::unique_ptr<minimal::mcp::McpClientManager> manager;
    std::unique_ptr<minimal::mcp::McpChannelSource> channel_source;
    std::shared_ptr<minimal::mcp::McpToolSource> tool_source;
    std::unique_ptr<minimal::mcp::McpPluginEndpoint> endpoint;
};

State& state() {
    static State instance;
    return instance;
}

void Warn(const PluginRegistrationContext& context, const std::string& message) {
    if (context.report_warning) {
        context.report_warning(message);
    } else {
        minimal::common::DefaultLogger().warn("mcp: {}", message);
    }
}

// A missing file is the normal "nothing configured" state; a malformed one
// degrades to nothing configured, loudly.
template <typename Config, typename Loader>
Config LoadOrEmpty(const PluginRegistrationContext& context, const char* file, Loader load) {
    try {
        return load(minimal::common::ConfigDir() / file);
    } catch (const std::exception& e) {
        Warn(context, std::string(file) + ": " + e.what() + "; nothing from it is configured");
        return Config{};
    }
}

}  // namespace

MINIMAL_PLUGIN_CONFIG_SCHEMA(
    minimal::common::PluginConfigLabel(minimal::common::kPluginConfigDirName, "mcp"),
    [](const nlohmann::json& config) {
        (void)minimal::mcp::ParseMcpLimits(config);
    })

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("mcp")

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const PluginRegistrationContext& context) {
    auto& s = state();
    if (s.manager) return;

    const auto limits = minimal::mcp::ParseMcpLimits(context.config);
    auto servers = LoadOrEmpty<minimal::config::McpsConfig>(context, "mcps.json",
                                                            minimal::config::LoadMcpsConfig);
    auto channels = LoadOrEmpty<minimal::config::ChannelsConfig>(
        context, "channels.json", minimal::config::LoadChannelsConfig);
    for (const auto& warning : minimal::mcp::ValidateMcpConfig(servers, channels)) {
        Warn(context, warning);
    }

    // The mcp__ namespace is this plugin's (the manager reserves it on
    // construction): its servers' tool groups live there, and no other
    // registrant may place names in it.
    auto* signals = context.services != nullptr
                        ? static_cast<minimal::agent::SignalService*>(
                              context.services->Find(minimal::agent::kSignalServiceName))
                        : nullptr;
    s.manager = std::make_unique<minimal::mcp::McpClientManager>(
        std::move(servers), registry, limits, minimal::common::DefaultLogger(),
        minimal::mcp::MakeChannelWiring(channels, signals));
    s.channel_source = std::make_unique<minimal::mcp::McpChannelSource>(*s.manager);
    s.tool_source = std::make_shared<minimal::mcp::McpToolSource>(
        *s.manager, minimal::common::DefaultLogger());
    s.endpoint = std::make_unique<minimal::mcp::McpPluginEndpoint>(*s.manager);

    // core.signals is core-provided before plugin load, so it can be used at
    // registration (unlike a plugin-provided service).
    if (signals != nullptr) {
        minimal::mcp::RegisterChannels(channels, *s.manager, *signals, *s.channel_source,
                                       [&context](const std::string& m) { Warn(context, m); });
    } else if (!channels.entries.empty()) {
        Warn(context, "channels.json configures channels, but this host has no signal service");
    }
}

void minimal_plugin_register_tool_sources(minimal::tools::ToolSourceRegistry& registry,
                                          const PluginRegistrationContext&) {
    if (state().tool_source) registry.Register(minimal::mcp::kMcpToolSourceName, state().tool_source);
}

void minimal_plugin_register_permission_models(minimal::tools::PermissionModelRegistry& registry,
                                               const PluginRegistrationContext&) {
    // The locality dimension asks the manager which transport a server uses.
    minimal::mcp::McpClientManager* manager = state().manager.get();
    registry.Register(minimal::mcp::MakeMcpPermissionModel(
        [manager](std::string_view server) -> std::optional<std::string> {
            return manager != nullptr ? manager->LocalityOf(std::string(server)) : std::nullopt;
        }));
}

void minimal_plugin_register_commands(minimal::commands::CommandRegistry& registry,
                                      const PluginRegistrationContext&) {
    if (state().manager) registry.Register(std::make_unique<minimal::mcp::McpCommand>(*state().manager));
}

void minimal_plugin_register_services(minimal::plugins::ServiceRegistry& registry,
                                      const PluginRegistrationContext&) {
    if (state().endpoint) {
        registry.Provide(minimal::plugins::PluginEndpointServiceName("mcp"), state().endpoint.get());
    }
}

void minimal_plugin_shutdown() {
    // Every session has ended. Destroying the manager stops the reaper and
    // tears every server process down, while the ToolRegistry and the signal
    // router it references still exist.
    auto& s = state();
    s.endpoint.reset();
    s.tool_source.reset();
    s.channel_source.reset();
    s.manager.reset();
}

}  // extern "C"
