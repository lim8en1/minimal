// prompt-editor -- the prompt editor (prompt_editor.hpp), in the required
// prompt_editor slot.

#include "minimal/client/sdk/client_plugin_abi.h"
#include "prompt_editor_factory.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("prompt-editor")

void minimal_client_plugin_register_prompt_editor(
    plugins::PromptEditorRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<minimal::client::prompt_editor::Factory>());
}

}  // extern "C"
