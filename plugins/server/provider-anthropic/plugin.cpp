// provider-anthropic -- the Anthropic Messages API provider.
//
// Registers a factory under the kind string "anthropic", which providers.json
// references via a provider entry's `backend` field -- no core change, no
// enum to keep in sync (ProviderFactoryRegistry is string-keyed precisely so
// a plugin can add a kind core has never heard of).
//
// base_url is expected WITHOUT a "/v1" suffix (unlike provider-openai's
// convention): the canonical value is "https://api.anthropic.com", and the
// provider appends "/v1/messages" itself.
//
// Thread-safety contract: instances are shared across concurrent sessions by
// the provider instance cache, so implementations must hold NO mutable state
// across calls -- one CURL handle per call, stream-reassembly state local to
// the call. Same contract provider-openai's implementation satisfies.

#include "minimal/plugins/plugin_abi.h"

#include "anthropic_provider.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("provider-anthropic")

void minimal_plugin_register_provider_factories(
    minimal::llm::ProviderFactoryRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("anthropic", [](const minimal::llm::ProviderConfig& config) {
        return std::make_unique<minimal::provider_anthropic::AnthropicProvider>(config);
    });
}

}  // extern "C"
