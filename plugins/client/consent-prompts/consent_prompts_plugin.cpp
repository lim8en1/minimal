// consent-prompts -- the permission and confirmation prompt renderers
// (consent_prompts.hpp).

#include "consent_prompts.hpp"
#include "minimal/client/sdk/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("consent-prompts")

void minimal_client_plugin_register_prompt_renderers(
    plugins::PromptRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    // Without a kit there is nothing to draw with; the kinds stay unclaimed
    // and the host refuses to start (they are required).
    if (context.widgets == nullptr) return;
    for (auto& renderer : minimal::client::consent_prompts::MakeRenderers(context.widgets)) {
        registry.Register(std::move(renderer));
    }
}

}  // extern "C"
