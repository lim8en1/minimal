#pragma once

// The `message_session` tool: send one signal to another same-server session.
//
// The FIRST consumer of the core.signals service (signal_service.hpp), and
// the bundled demonstration of the plugin-producer shape: core owns the
// router, the subscriptions, and the injection machinery; this plugin only
// DELIVERS, through the service it Finds lazily at execute time.
//
// Category FileWrite, and that is deliberate: the tool reaches no filesystem
// and no process, but its default must be Ask -- its content lands in another
// session's context unbidden, and the receiving session's subscription is the
// operator's opt-in on the RECEIVE side, not on what any sender may push.
// "Inject into another agent's context" is a write, just not to a file.
//
// DEGRADE: a host without the service (null registry or Find miss) answers
// with a clear error result -- a runtime condition, not a crash.

#include "minimal/plugins/plugin_abi.h"
#include "minimal/tools/tool.hpp"

namespace minimal::signals_message {

std::unique_ptr<tools::Tool> MakeMessageSessionTool(
    const minimal::plugins::ServiceRegistry* services);

}  // namespace minimal::signals_message
