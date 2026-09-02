#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/http_client.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::provider_anthropic {

// Holds only its ProviderConfig and a stateless HttpClient value -- no
// mutable state survives a call, per LlmProvider's thread-safety contract;
// instances are shared across concurrent sessions by the provider instance
// cache. Same shape as provider-openai::OpenAiProvider.
class AnthropicProvider : public llm::LlmProvider {
public:
    explicit AnthropicProvider(llm::ProviderConfig config) : config_(std::move(config)) {}

    llm::ChatResponse Complete(const llm::ChatRequest& request) override;
    llm::ChatResponse StreamComplete(const llm::ChatRequest& request,
                                     const llm::StreamEventSink& sink) override;

private:
    llm::HttpRequest BuildHttpRequest(std::string body, bool streaming) const;

    llm::ProviderConfig config_;
    llm::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace minimal::provider_anthropic
