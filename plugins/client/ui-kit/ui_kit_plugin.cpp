// ui-kit -- the bundled widget kit (ui_kit.hpp).

#include <memory>

#include "minimal/client/sdk/client_plugin_abi.h"
#include "ui_kit.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("ui-kit")

void minimal_client_plugin_register_widget_kit(plugins::WidgetKitRegistry& registry,
                                               const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<minimal::client::ui_kit::UiKit>());
}

}  // extern "C"
