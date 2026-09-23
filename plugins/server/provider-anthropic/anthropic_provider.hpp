#pragma once

#include <nlohmann/json.hpp>

#include "minimal/llm/provider.hpp"
#include "minimal/net/http_client.hpp"

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
    // Signed thinking is replayed, and newer Claude models reject a replay
    // whose prefix changed -- which every keep-tail compaction does.
    bool BindsThinkingToPrefix() const override { return true; }

private:
    net::HttpRequest BuildHttpRequest(std::string body, bool streaming,
                                      const std::string& session_id) const;

    llm::ProviderConfig config_;
    net::HttpClient http_;  // stateless -- see HttpClient's own contract
};

}  // namespace minimal::provider_anthropic
