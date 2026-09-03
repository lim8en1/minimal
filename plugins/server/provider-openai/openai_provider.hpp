#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/provider.hpp"
#include "minimal/net/http_client.hpp"

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
    net::HttpRequest BuildHttpRequest(std::string body, bool streaming) const;

    llm::ProviderConfig config_;
    net::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace minimal::provider_openai
