// ask-view -- the client half of the tool-ask plugin pair. Two registrations,
// both keyed by kind "ask", neither by tool name (an aliased `ask_user`
// keeps both):
//
//   - the "ask" PROMPT renderer: the modal the server's ask_user parks on
//     (ToolContext::prompt_user, kind "ask");
//   - the "ask" DISPLAY renderer: the transcript row recording what was asked
//     and answered, expanded by default.
//
// With this plugin disabled the host answers every ask UNAVAILABLE and the
// model is told no one can answer; the row falls back to the plain text row.

#include <memory>

#include "ask_display.hpp"
#include "ask_modal.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("ask-view")

void minimal_client_plugin_register_prompt_renderers(
    plugins::PromptRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // No widget kit offered (a test host): no modal to draw, and an
    // unclaimed kind answers UNAVAILABLE rather than leaving the tool parked.
    if (context.widgets == nullptr) return;
    registry.Register(std::make_unique<minimal::ask_view::AskPromptRenderer>(context.widgets));
}

void minimal_client_plugin_register_display_renderers(
    plugins::DisplayRendererRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<minimal::ask_view::AskDisplayRenderer>());
}

}  // extern "C"
