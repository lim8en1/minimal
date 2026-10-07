// selection-view -- the agent, model and subagent-model choosers
// (selection_view.hpp).

#include <memory>

#include "minimal/client/sdk/client_plugin_abi.h"
#include "selection_view.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("selection-view")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    // Lists come from the server and choices go to the host: without either
    // (a test host) there is nothing to choose.
    if (context.ui == nullptr || context.actions == nullptr || context.widgets == nullptr ||
        context.client_api == nullptr) {
        return;
    }
    const minimal::selection_view::Seams seams{context.ui, context.actions, context.widgets,
                                               context.client_api};
    registry.Register(std::make_unique<minimal::selection_view::AgentsView>(seams));
    registry.Register(std::make_unique<minimal::selection_view::ModelsView>(seams, false));
    registry.Register(std::make_unique<minimal::selection_view::ModelsView>(seams, true));
}

}  // extern "C"
