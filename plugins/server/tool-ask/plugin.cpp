// tool-ask -- the `ask_user` tool.
//
// One tool. It turns a structured model request ("ask the operator which of
// these approaches to take") into a blocking round-trip to the human via
// ToolContext::prompt_user -- core's opaque plugin prompt, kind "ask" -- and
// returns the answer as the tool result. The client half is the `ask-view`
// client plugin, which registers the "ask" prompt renderer and the "ask"
// display renderer for the transcript row.
//
// The tool itself holds no state and reaches nothing session-scoped on its
// own: every interactive capability arrives in the per-invocation
// ToolContext, exactly like schedule_injection and friends. A session with
// no prompt broker (headless fixtures, tests) hands the tool a null handle
// and it reports "unavailable" instead of parking a worker nobody can
// answer.
//
// Permission posture: ReadOnly, like the skill tool. The tool touches no
// filesystem, process, or network; its whole effect is the prompt it raises,
// so an Ask category would double-prompt and an Execute category would gate
// a question. See ARCHITECTURE.md "Asking the user".

#include "minimal/plugins/plugin_abi.h"

#include "ask_tool.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tool-ask")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    (void)context;  // no config yet; the tool reads nothing from plugins.json

    registry.Register(minimal::tool_ask::MakeAskTool());
}

}  // extern "C"
