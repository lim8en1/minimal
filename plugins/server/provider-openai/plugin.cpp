// provider-openai -- the OpenAI chat-completions provider.
//
// Registers a factory under the kind string "openai", which providers.json
// references. There is no ProviderKind enum: the registry is string-keyed
// precisely so a plugin can add a kind core has never heard of.
//
// Targets any OpenAI-compatible endpoint via base_url, including local
// llama.cpp / vLLM servers -- which is why llama.cpp needs no dedicated provider.
//
// Thread-safety contract: instances are shared across concurrent sessions by the
// provider instance cache, so implementations must hold NO mutable state across
// calls -- one CURL handle per call, stream-reassembly state local to the call.

#include "minimal/plugins/plugin_abi.h"

#include "openai_provider.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "provider-openai"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_provider_factories(minimal::llm::ProviderFactoryRegistry& registry) {
    registry.Register("openai", [](const minimal::llm::ProviderConfig& config) {
        return std::make_unique<minimal::provider_openai::OpenAiProvider>(config);
    });
}

}  // extern "C"
