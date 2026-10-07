// tab-bar -- the row of tabs above the chat (tab_bar.hpp), in the optional
// tab_bar slot.

#include <memory>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "tab_bar.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("tab-bar")

void minimal_client_plugin_register_tab_bar(plugins::TabBarRegistry& registry,
                                            const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<minimal::client::tab_bar::Bar>());
}

}  // extern "C"
