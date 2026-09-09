// tool-ask -- the `ask_user` tool.
//
// One tool. It turns a structured model request ("ask the operator which of
// these approaches to take") into a blocking round-trip to the human via
// ToolContext::ask_user -- the session's AskBroker wiring -- and returns the
// answer as the tool result.
//
// The tool itself holds no state and reaches nothing session-scoped on its
// own: every interactive capability arrives in the per-invocation
// ToolContext, exactly like schedule_injection and friends. A session with
// no ask broker (headless fixtures, tests) hands the tool a null handle and
// it degrades to an error result instead of parking a worker nobody can
// answer.
//
// Permission posture: ReadOnly, like the skill tool. The tool touches no
// filesystem, process, or network; its whole effect is the prompt it raises,
// so an Ask category would double-prompt and an Execute category would gate
// a question. See ARCHITECTURE.md "Asking the user".

#include "minimal/plugins/plugin_abi.h"

#include "ask_tool.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tool-ask"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    (void)context;  // no config yet; the tool reads nothing from plugins.json

    registry.Register(minimal::tool_ask::MakeAskTool());
}

}  // extern "C"
