// chat-renderers -- the bundled look of every chat row and display payload
// kind (chat_renderers.hpp), as fallbacks.

#include "chat_renderers.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("chat-renderers")

void minimal_client_plugin_register_message_renderers(
    plugins::MessageRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    if (context.markdown == nullptr) return;
    minimal::client::chat_renderers::RegisterMessageRenderers(registry, *context.markdown);
}

void minimal_client_plugin_register_display_renderers(
    plugins::DisplayRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    if (context.markdown == nullptr || context.highlighter == nullptr) return;
    minimal::client::chat_renderers::RegisterDisplayRenderers(registry, *context.markdown,
                                                               *context.highlighter);
}

}  // extern "C"
