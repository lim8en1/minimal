#include "embedding_http.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::memory {
namespace {

// OpenAI-shape POST {api_base}/embeddings {model, input[]} ->
// {"data": [{"embedding": [...], "index": 0}, ...]}. llama.cpp server
// (/v1/embeddings), vLLM and OpenAI all serve this shape. The index field is
// trusted over response order because the spec defines it.

}  // namespace

HttpEmbeddingFunction::HttpEmbeddingFunction(MemorySettings settings, net::HttpClient http)
    : settings_(std::move(settings)), http_(std::move(http)) {}

std::vector<Embedding> HttpEmbeddingFunction::Embed(const std::vector<std::string>& inputs) {
    if (inputs.empty()) return {};

    std::vector<Embedding> out;
    out.reserve(inputs.size());
    const std::size_t batch_size = static_cast<std::size_t>(std::max(1, settings_.batch_size));
    for (std::size_t start = 0; start < inputs.size(); start += batch_size) {
        const std::size_t end = std::min(inputs.size(), start + batch_size);
        auto chunk = EmbedChunk(inputs, start, end);
        out.insert(out.end(), std::make_move_iterator(chunk.begin()),
                  std::make_move_iterator(chunk.end()));
    }
    return out;
}

std::vector<Embedding> HttpEmbeddingFunction::EmbedChunk(const std::vector<std::string>& inputs,
                                                         std::size_t start, std::size_t end) {
    const std::vector<std::string> chunk(inputs.begin() + static_cast<std::ptrdiff_t>(start),
                                         inputs.begin() + static_cast<std::ptrdiff_t>(end));

    nlohmann::json body;
    body["model"] = settings_.model;
    body["input"] = chunk;
    // Two sentences of input encoding are worth stating: OpenAI's embeddings
    // API accepts either a string or an array of strings; we always send the
    // array so batching is uniform across backends.
    std::vector<net::HttpHeader> request_headers;
    request_headers.push_back({"Content-Type", "application/json"});
    request_headers.push_back({"Accept", "application/json"});
    if (!settings_.api_key_env.empty()) {
        // Resolved per call, not once at construction: a rotated key without a
        // server restart is worth the getenv cost. tools-web resolves at
        // startup and fails loud on an unset name; memory resolves at call
        // time and reports through the same std::runtime_error channel.
        if (const char* key = std::getenv(settings_.api_key_env.c_str())) {
            if (*key != '\0') request_headers.push_back({"Authorization", "Bearer " + std::string(key)});
        }
    }

    net::HttpRequest request;
    request.url = settings_.api_base + "/embeddings";
    request.headers = std::move(request_headers);
    // replace, not the default (throwing): the text being embedded is
    // whatever memory_store ingested, which is not guaranteed valid UTF-8 --
    // a Latin-1 file must degrade this one embedding call, not throw out of
    // the whole reindex batch and (via MarkEmbedFailed) mark every OTHER row
    // in it failed too.
    request.body = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    request.request_timeout = std::chrono::milliseconds(settings_.timeout_ms);
    request.connect_timeout = std::chrono::milliseconds(5000);

    const net::HttpResponse response = http_.Post(request);
    if (response.status < 200 || response.status >= 300) {
        throw std::runtime_error("embedding backend HTTP " + std::to_string(response.status) +
                                 " from " + settings_.api_base + ": " +
                                 (response.body.size() > 256
                                      ? response.body.substr(0, 256) + "…"
                                      : response.body));
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(response.body);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("embedding backend returned invalid JSON: ") +
                                 error.what());
    }
    const auto data = parsed.find("data");
    if (data == parsed.end() || !data->is_array() || data->size() != chunk.size()) {
        throw std::runtime_error("embedding backend response: expected data[" +
                                 std::to_string(chunk.size()) + "], got " +
                                 (data == parsed.end() ? "no data" : data->dump().substr(0, 120)));
    }

    std::vector<Embedding> out(chunk.size());
    for (const auto& entry : *data) {
        if (!entry.is_object() || !entry.contains("embedding") || !entry["embedding"].is_array() ||
            !entry.contains("index") || !entry["index"].is_number_integer()) {
            throw std::runtime_error("embedding backend response: malformed data entry");
        }
        const std::size_t index = entry["index"].get<std::size_t>();
        if (index >= out.size()) {
            throw std::runtime_error("embedding backend response: index " +
                                     std::to_string(index) + " out of range");
        }
        Embedding& embedding = out[index];
        embedding.values.reserve(entry["embedding"].size());
        for (const auto& value : entry["embedding"]) {
            if (!value.is_number()) {
                throw std::runtime_error("embedding backend response: non-numeric embedding value");
            }
            embedding.values.push_back(value.get<float>());
        }
        embedding.dim = static_cast<int>(embedding.values.size());
    }
    for (const auto& embedding : out) {
        if (embedding.dim == 0) {
            throw std::runtime_error("embedding backend response: empty embedding vector");
        }
    }
    return out;
}

std::string HttpEmbeddingFunction::Describe() const {
    return "http " + settings_.api_base + " (" + settings_.model + ")";
}

std::string HttpEmbeddingFunction::Signature() const { return SignatureFor(settings_); }

std::string HttpEmbeddingFunction::SignatureFor(const MemorySettings& settings) {
    return "http:" + settings.api_base + ":" + settings.model;
}

}  // namespace minimal::memory
