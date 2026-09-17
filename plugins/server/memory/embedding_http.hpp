#pragma once

#include <string>
#include <vector>

#include "embedding.hpp"
#include "memory_settings.hpp"
#include "minimal/net/http_client.hpp"

namespace minimal::memory {

// OpenAI-shape /embeddings client. Stateless apart from settings -- net::HttpClient
// opens its own CURL handle per call, so concurrent Embed() calls are safe.
class HttpEmbeddingFunction final : public EmbeddingFunction {
public:
    HttpEmbeddingFunction(MemorySettings settings, net::HttpClient http);

    std::vector<Embedding> Embed(const std::vector<std::string>& inputs) override;
    std::string Describe() const override;
    std::string Signature() const override;

private:
    MemorySettings settings_;
    net::HttpClient http_;
};

}  // namespace minimal::memory
