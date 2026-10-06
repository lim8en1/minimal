// session-view -- the session and subagent pickers (session_view.hpp).

#include <memory>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "session_view.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("session-view")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    // Lists come from the server and opening is the host's: without either
    // (a test host) there is nothing to show.
    if (context.ui == nullptr || context.actions == nullptr || context.widgets == nullptr ||
        context.client_api == nullptr || context.focus == nullptr) {
        return;
    }
    const minimal::session_view::Seams seams{context.ui, context.actions, context.widgets,
                                             context.client_api, context.focus};
    registry.Register(std::make_unique<minimal::session_view::SessionsView>(seams));
    registry.Register(std::make_unique<minimal::session_view::SubagentsView>(seams));
}

}  // extern "C"
