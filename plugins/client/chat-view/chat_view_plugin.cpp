// chat-view -- the chat as the TUI's body (chat_surface.hpp), in the required
// chat_surface slot.

#include <memory>

#include "chat_surface.hpp"
#include "minimal/client/sdk/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("chat-view")

void minimal_client_plugin_register_chat_surface(
    plugins::ChatSurfaceRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    namespace chat_view = minimal::client::chat_view;
    registry.Register(std::make_unique<chat_view::Surface>(
        chat_view::ParseSurfaceConfig(context.config), context.ui));
}

}  // extern "C"
