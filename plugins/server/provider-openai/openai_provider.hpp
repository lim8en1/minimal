#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/http_client.hpp"
#include "minimal/llm/provider.hpp"

namespace minimal::provider_openai {

// Holds only its ProviderConfig and a stateless HttpClient value -- no
// mutable state survives a call, per LlmProvider's thread-safety contract;
// instances are shared across concurrent sessions by the (not-yet-built)
// provider instance cache.
class OpenAiProvider : public llm::LlmProvider {
public:
    explicit OpenAiProvider(llm::ProviderConfig config) : config_(std::move(config)) {}

    llm::ChatResponse Complete(const llm::ChatRequest& request) override;
    llm::ChatResponse StreamComplete(const llm::ChatRequest& request, const llm::StreamEventSink& sink) override;

private:
    llm::HttpRequest BuildHttpRequest(const nlohmann::json& body, bool streaming) const;

    llm::ProviderConfig config_;
    llm::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace minimal::provider_openai
