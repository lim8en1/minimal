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

#include "minimal/common/abi_tag.hpp"

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "provider-openai"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

// TODO: minimal_plugin_register_provider_factories -- "openai" -> OpenAiProvider,
// using core's HttpClient + SseParser from the utilities half of the ABI.

}  // extern "C"
