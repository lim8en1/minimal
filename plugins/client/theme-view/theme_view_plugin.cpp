// theme-view -- the theme picker and /theme (theme_view.hpp).

#include <memory>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "theme_view.hpp"

namespace plugins = minimal::client::plugins;

namespace {

// Without the host's reads and actions there is nothing to switch (a test
// host offers none).
bool Hosted(const plugins::ClientPluginRegistrationContext& context) {
    return context.ui != nullptr && context.actions != nullptr && context.widgets != nullptr;
}

minimal::theme_view::Seams SeamsOf(const plugins::ClientPluginRegistrationContext& context) {
    return {context.ui, context.actions, context.widgets};
}

}  // namespace

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("theme-view")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    if (!Hosted(context)) return;
    registry.Register(std::make_unique<minimal::theme_view::ThemeView>(SeamsOf(context)));
}

void minimal_client_plugin_register_client_commands(
    plugins::ClientCommandRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    if (!Hosted(context)) return;
    registry.Register(std::make_unique<minimal::theme_view::ThemeCommand>(SeamsOf(context)));
}

}  // extern "C"
