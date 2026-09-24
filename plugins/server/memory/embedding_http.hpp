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
    // The same signature without constructing a client: the runtime knows
    // which rows need re-embedding before the backend exists.
    static std::string SignatureFor(const MemorySettings& settings);

private:
    // One POST for inputs[start, end). Split out so Embed() can chunk by
    // settings_.batch_size -- a caller (the reindex drain, a large ingest)
    // may pass far more inputs than one request should carry.
    std::vector<Embedding> EmbedChunk(const std::vector<std::string>& inputs, std::size_t start,
                                      std::size_t end);

    MemorySettings settings_;
    net::HttpClient http_;
};

}  // namespace minimal::memory
