// signals-message -- the bundled message_session tool, the first producer on
// the core.signals service. Ships ENABLED (unlike session-titler it spends no
// tokens unprompted and its default permission is Ask): cross-session
// messaging is the v1 of agent-to-agent exchange, gated twice -- the sender
// by this tool's permission mode, the receiver by its own "sessions"
// subscription at start.

#include <memory>

#include "minimal/plugins/plugin_abi.h"

#include "message_tool.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "signals-message"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    // The service registry is resolved lazily at execute time (Find), so a
    // raw pointer travels inside the tool -- the titler's pattern. The
    // registry is main-owned and outlives every session and tool call.
    registry.Register(
        minimal::signals_message::MakeMessageSessionTool(context.services));
}

}  // extern "C"
