// markdown-md4c -- the bundled markdown engine (md4c_engine.hpp).

#include "md4c_engine.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("markdown-md4c")

void minimal_client_plugin_register_markdown_engine(
    plugins::MarkdownEngineRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    minimal::client::markdown_md4c::RegisterEngine(registry, context.highlighter, context.ui);
}

}  // extern "C"
