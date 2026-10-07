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

#include <memory>

#include "anthropic_model_catalog.hpp"
#include "anthropic_provider.hpp"
#include "anthropic_variant_params.hpp"

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("provider-anthropic")

void minimal_plugin_register_provider_factories(
    minimal::llm::ProviderFactoryRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("anthropic", [](const minimal::llm::ProviderConfig& config) {
        return std::make_unique<minimal::provider_anthropic::AnthropicProvider>(config);
    });
}

// GET /v1/models: per-model context window, output cap and capabilities, so
// a `"discover": true` entry needs no per-model config. Same kind string as
// the factory above; stateless, so one shared instance serves every resolver.
void minimal_plugin_register_model_catalogs(
    minimal::llm::ModelCatalogRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("anthropic",
                      std::make_shared<minimal::provider_anthropic::AnthropicModelCatalog>(
                          minimal::net::HttpClient{}));
}

// The "@effort" variant wire-shape provider (ARCHITECTURE.md, "Core vs.
// plugin surface") -- a second, sibling registration under the SAME kind
// string, consulted by core's VariantParametersFor dispatcher for both the
// static "variants": [...] list form (parse time) and the discovered-model
// variant path (ResolveVariant, lazy). See variant_params.hpp for why this
// is a separate registry rather than a capability query on LlmProvider.
void minimal_plugin_register_variant_params(
    minimal::llm::VariantParamsRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("anthropic", std::make_shared<minimal::provider_anthropic::AnthropicVariantParams>());
}

// A static model's "thinking": "adaptive" | "budget" | "none" -- the
// capability report discovery would have stored, declared in providers.json.
void minimal_plugin_register_provider_config_extensions(
    minimal::config::ProviderConfigExtensions& extensions,
    const minimal::plugins::PluginRegistrationContext&) {
    extensions.AddTranslator(std::make_shared<minimal::provider_anthropic::AnthropicThinkingTranslator>());
}

}  // extern "C"
