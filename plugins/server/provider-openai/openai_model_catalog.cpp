#include "openai_model_catalog.hpp"

#include <stdexcept>

#include "openai_error_mapping.hpp"

namespace minimal::provider_openai {
namespace {

using nlohmann::json;

// A listing is a bounded, latency-sensitive call made from paths that serve
// users (--model resolution, /providers) -- not from the chat path. A hung
// backend must therefore not wedge a caller for the chat-length request
// timeout; ten seconds bounds the worst case while local servers answer in
// milliseconds.
constexpr std::chrono::milliseconds kListRequestTimeout{10000};

// <base_url>/models, tolerating a trailing slash on the configured url.
std::string ModelsUrl(const std::string& base_url) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/models";
    return url;
}

// The first numeric field present, in per-server precedence order. All three
// names mean the same thing; the winner is whichever the backend serves.
std::optional<std::int64_t> ContextWindow(const json& entry) {
    static const char* kFields[] = {"context_length", "max_model_len"};
    for (const char* field : kFields) {
        if (entry.contains(field) && entry.at(field).is_number()) {
            return entry.at(field).get<std::int64_t>();
        }
    }
    if (entry.contains("meta") && entry.at("meta").is_object()) {
        const json& meta = entry.at("meta");
        if (meta.contains("n_ctx_train") && meta.at("n_ctx_train").is_number()) {
            return meta.at("n_ctx_train").get<std::int64_t>();
        }
    }
    return std::nullopt;
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
    for (const auto& entry : doc.at("data")) {
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

std::vector<llm::DiscoveredModel> OpenAiModelCatalog::List(const llm::ProviderConfig& config) const {
    net::HttpRequest request;
    request.url = ModelsUrl(config.base_url);
    request.connect_timeout = config.connect_timeout;
    request.request_timeout = kListRequestTimeout;
    if (!config.api_key.empty()) {
        request.headers.push_back({"Authorization", "Bearer " + config.api_key});
    }

    const net::HttpResponse response = http_.Get(request);
    if (response.status < 200 || response.status >= 300) {
        // Classified the same way a chat call against the same backend would
        // be, so a 401 from a stale key reads as an auth error, not "catalog
        // failed" -- and Retry-After on a 429 survives into the message.
        throw ClassifyHttpFailure(response.status, response.body, response.headers);
    }
    return ParseModelList(response.body);
}

}  // namespace minimal::provider_openai
