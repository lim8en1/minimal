// config-view -- plugin configuration in the TUI: the "config" modal view and
// its /config command. It talks to the server only through the generic
// PluginCall channel, to core's `plugins` endpoint; the form itself is
// client core's reusable client::MakeSchemaForm.

#include <memory>

#include "config_view.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

namespace {

// Registered by the views hook, used by the command hook (the loader calls
// views first).
minimal::config_view::ConfigView* g_view = nullptr;

}  // namespace

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("config-view")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    // No data path or widget kit offered (a test host): nothing to show.
    if (context.client_api == nullptr || context.widgets == nullptr) return;
    auto view = std::make_unique<minimal::config_view::ConfigView>(context.client_api,
                                                                   context.widgets, context.ui);
    g_view = view.get();
    registry.Register(std::move(view));
}

void minimal_client_plugin_register_client_commands(
    plugins::ClientCommandRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    if (g_view == nullptr) return;
    registry.Register(std::make_unique<minimal::config_view::ConfigCommand>(g_view, context.ui));
}

}  // extern "C"
