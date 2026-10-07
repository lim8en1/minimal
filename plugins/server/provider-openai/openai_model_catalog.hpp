#pragma once

// Model catalog for OpenAI-shaped backends (track N, docs/EXTENSIBILITY.md
// §2.6). One implementation covers llama.cpp, vLLM and OpenRouter alike: the
// shape is identical across OpenAI-compatible servers -- a GET on the same
// base URL -- and the per-server differences are just the field that carries
// the context window:
//
//   OpenRouter:        data[].context_length
//   llama.cpp:         data[].meta.n_ctx_train
//   vLLM:              data[].max_model_len
//   LiteLLM gateways:  data[].max_input_tokens (+ data[].max_output_tokens
//                      as the output cap)
//
// Any of them may be absent -- a model without a discoverable window degrades
// to nullopt (compaction stays off for it), never to a guessed number.

#include <string>
#include <vector>

#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/net/http_client.hpp"

namespace minimal::provider_openai {

// Pure: parses an OpenAI-shaped /models response body. Throws std::runtime_error
// on a body that is not an object with a "data" array, or an entry without a
// usable "id" -- the caller negative-caches the failure rather than serving a
// partial list as if it were complete.
std::vector<llm::DiscoveredModel> ParseModelList(const std::string& body);

// The transport half. Stateless, like the provider: List() may be called
// concurrently from resolvers on several session workers and the command pool.
class OpenAiModelCatalog : public llm::ModelCatalog {
public:
    explicit OpenAiModelCatalog(const net::HttpClient& http) : http_(http) {}

    std::vector<llm::DiscoveredModel> List(const llm::ProviderConfig& config) const override;

private:
    net::HttpClient http_;
};

}  // namespace minimal::provider_openai
