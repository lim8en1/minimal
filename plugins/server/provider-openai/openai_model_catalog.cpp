#include "openai_model_catalog.hpp"

#include <limits>
#include <string_view>
#include <stdexcept>

#include "openai_error_mapping.hpp"
#include "minimal/common/http_status.hpp"
#include "minimal/llm/policy.hpp"

namespace minimal::provider_openai {
namespace {

using nlohmann::json;

// A listing is a bounded, latency-sensitive call made from paths that serve
// users (--model resolution, the /model picker's ListModels) -- not from the
// chat path. A hung
// backend must therefore not wedge a caller for the chat-length request
// timeout; a short bound caps the worst case while local servers answer in
// milliseconds (llm::kListModelsTimeout).

// <base_url>/<endpoint>, tolerating a trailing slash on the configured url.
std::string EndpointUrl(const std::string& base_url, std::string_view endpoint) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += '/';
    url += endpoint;
    return url;
}

// One discovery GET against `endpoint`, 2xx-checked. Classified the same way
// a chat call against the same backend would be, so a 401 from a stale key
// reads as an auth error, not "catalog failed" -- and Retry-After on a 429
// survives into the message.
std::string Get(const net::HttpClient& http, const llm::ProviderConfig& config,
                std::string_view endpoint) {
    net::HttpRequest request;
    request.url = EndpointUrl(config.base_url, endpoint);
    request.connect_timeout = config.connect_timeout;
    request.request_timeout = llm::kListModelsTimeout;
    if (const auto key = llm::RequestApiKey(config)) {
        request.headers.push_back({"Authorization", "Bearer " + *key});
    }
    // Same headers the chat path sends (see OpenAiProvider::BuildHttpRequest):
    // a header-auth gateway (tenant id, routing tags) 401s every discovery
    // call that carries only the bearer token.
    for (const auto& [name, value] : config.headers) {
        request.headers.push_back({name, value});
    }

    net::HttpResponse response = http.Get(request);
    if (!common::IsSuccessStatus(response.status)) {
        throw ClassifyHttpFailure(response.status, response.body, response.headers);
    }
    return std::move(response.body);
}

// The first numeric field present, in per-server precedence order. The names
// mean the same thing; the winner is whichever the backend serves.
// "max_input_tokens" (the LiteLLM-proxy spelling) is last: the input limit is
// what compaction measures, but where a backend names the full window that is
// the more specific answer.
// A non-POSITIVE value is treated as absent: a negative context window is a
// misparse or a hostile entry, and storing it would make the compaction
// trigger fire on every turn (occupancy < negative threshold is never true).
std::optional<std::int64_t> ContextWindow(const json& entry) {
    static const char* kFields[] = {"context_length", "max_model_len", "max_input_tokens"};
    // is_number_integer(), not is_number(): a catalog entry reporting
    // "context_length": 131072.9 is a misparse the same as a negative one,
    // and is_number() let get<int64_t>() silently truncate it instead of
    // rejecting it the way SafeTokenCount already does for usage fields.
    for (const char* field : kFields) {
        if (entry.contains(field) && entry.at(field).is_number_integer()) {
            const std::int64_t value = entry.at(field).get<std::int64_t>();
            if (value > 0) return value;
        }
    }
    if (entry.contains("meta") && entry.at("meta").is_object()) {
        const json& meta = entry.at("meta");
        if (meta.contains("n_ctx_train") && meta.at("n_ctx_train").is_number_integer()) {
            const std::int64_t value = meta.at("n_ctx_train").get<std::int64_t>();
            if (value > 0) return value;
        }
    }
    return std::nullopt;
}

// A positive int-ranged integer field of `node`, or nullopt.
std::optional<int> PositiveIntField(const json& node, const char* field) {
    if (node.contains(field) && node.at(field).is_number_integer()) {
        const std::int64_t value = node.at(field).get<std::int64_t>();
        if (value > 0 && value <= std::numeric_limits<int>::max()) {
            return static_cast<int>(value);
        }
    }
    return std::nullopt;
}

// The output cap, when the backend reports one. Two spellings: OpenRouter's
// `top_provider.max_completion_tokens` and the LiteLLM-proxy top-level
// `max_output_tokens`. A plain OpenAI-shape /models body has neither and
// yields nullopt, same as the absent-window case. The same positive-only rule
// as ContextWindow: a non-positive value is a misparse or a hostile entry,
// and storing it would cap every request at nothing.
std::optional<int> MaxCompletionTokens(const json& entry) {
    if (const auto cap = PositiveIntField(entry, "max_output_tokens")) return cap;
    if (!entry.contains("top_provider") || !entry.at("top_provider").is_object()) {
        return std::nullopt;
    }
    return PositiveIntField(entry.at("top_provider"), "max_completion_tokens");
}

}  // namespace

std::vector<llm::DiscoveredModel> ParseModelList(const std::string& body) {
    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::exception&) {
        throw std::runtime_error("model list: response is not valid JSON");
    }
    if (!doc.is_object() || !doc.contains("data") || !doc.at("data").is_array()) {
        throw std::runtime_error("model list: expected an object with a \"data\" array");
    }

    std::vector<llm::DiscoveredModel> out;
    out.reserve(doc.at("data").size());
    for (auto& entry : doc.at("data")) {
        if (!entry.is_object()) continue;
        if (!entry.contains("id") || !entry.at("id").is_string() ||
            entry.at("id").get<std::string>().empty()) {
            // An entry without an id cannot be addressed as <provider>/<model>;
            // skipping it is honest -- failing the whole list over one malformed
            // row would take every good model down with it.
            continue;
        }

        llm::DiscoveredModel model;
        model.id = entry.at("id").get<std::string>();
        model.context_window_tokens = ContextWindow(entry);
        model.max_tokens = MaxCompletionTokens(entry);
        // Everything the backend said about the model minus the id: pricing,
        // quantization, ... -- opaque to core, free for a future consumer.
        // MOVED, not copied: OpenRouter entries carry multi-KB descriptions
        // and pricing, and a 1000-model list under the 8 MiB body cap made
        // this a second full DOM of the catalog per discovery fetch.
        json metadata = std::move(entry);
        metadata.erase("id");
        model.metadata = std::move(metadata);
        out.push_back(std::move(model));
    }
    return out;
}

std::map<std::string, json> ParseModelInfo(const std::string& body) {
    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::exception&) {
        throw std::runtime_error("model info: response is not valid JSON");
    }
    if (!doc.is_object() || !doc.contains("data") || !doc.at("data").is_array()) {
        throw std::runtime_error("model info: expected an object with a \"data\" array");
    }

    std::map<std::string, json> out;
    for (auto& entry : doc.at("data")) {
        if (!entry.is_object()) continue;
        // Same rule as ParseModelList's id-less row: a row that cannot be
        // matched to a listed id is skipped, not fatal.
        if (!entry.contains("model_name") || !entry.at("model_name").is_string() ||
            entry.at("model_name").get<std::string>().empty()) {
            continue;
        }
        if (!entry.contains("model_info") || !entry.at("model_info").is_object()) continue;
        // MOVED, not copied: a gateway's info body runs to megabytes.
        out.emplace(entry.at("model_name").get<std::string>(), std::move(entry.at("model_info")));
    }
    return out;
}

std::size_t MergeModelInfo(std::vector<llm::DiscoveredModel>& models,
                           std::map<std::string, json> info) {
    std::size_t enriched = 0;
    for (auto& model : models) {
        const auto it = info.find(model.id);
        if (it == info.end()) continue;  // the listing's row stands alone
        model.metadata["model_info"] = std::move(it->second);
        ++enriched;
    }
    return enriched;
}

std::vector<llm::DiscoveredModel> OpenAiModelCatalog::List(const llm::ProviderConfig& config) const {
    std::vector<llm::DiscoveredModel> models = ParseModelList(Get(http_, config, "models"));
    if (!config.fetch_model_info) return models;

    // The opt-in enrichment DEGRADES: a 404 (plain vLLM/llama.cpp), an auth
    // failure scoped to this endpoint, a malformed body -- all leave the
    // listing standing. The endpoint is a bonus; its absence must not read
    // as "the backend serves nothing".
    try {
        MergeModelInfo(models, ParseModelInfo(Get(http_, config, "model/info")));
    } catch (const std::exception&) {
    }
    return models;
}

}  // namespace minimal::provider_openai
