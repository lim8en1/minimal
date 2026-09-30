// mcp-view -- the client half of the mcp plugin pair: the "mcp" modal view
// the server's bare `/mcp` opens (view_hint "mcp"). It talks to the server
// only through the generic PluginCall channel ("mcp.endpoint"); with the
// server-side mcp plugin disabled, the view says so instead of listing.

#include <memory>

#include "mcp_picker.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("mcp-view")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    // No data path offered (a test host): nothing this view could show.
    if (context.client_api == nullptr) return;
    registry.Register(std::make_unique<minimal::mcp_view::McpPickerView>(context.client_api, context.ui));
}

}  // extern "C"
