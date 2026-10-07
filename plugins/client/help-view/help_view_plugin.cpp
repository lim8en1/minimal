// help-view -- the key bindings and /-commands overlays (help_view.hpp).

#include <memory>

#include "help_view.hpp"
#include "minimal/client/sdk/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

namespace {

// Everything the views draw comes from the host; without it there is
// nothing to show (a test host offers none).
bool Hosted(const plugins::ClientPluginRegistrationContext& context) {
    return context.ui != nullptr && context.actions != nullptr && context.widgets != nullptr;
}

}  // namespace

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("help-view")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    if (!Hosted(context)) return;
    const minimal::help_view::Seams seams{context.ui, context.actions, context.widgets};
    registry.Register(std::make_unique<minimal::help_view::KeymapView>(seams));
    registry.Register(std::make_unique<minimal::help_view::CommandsView>(seams));
}

void minimal_client_plugin_register_client_commands(
    plugins::ClientCommandRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    if (!Hosted(context)) return;
    registry.Register(std::make_unique<minimal::help_view::KeymapCommand>(context.ui));
    registry.Register(std::make_unique<minimal::help_view::HelpCommand>(context.ui));
}

}  // extern "C"
