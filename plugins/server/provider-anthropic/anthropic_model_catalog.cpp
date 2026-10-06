#include "anthropic_model_catalog.hpp"

#include <limits>
#include <stdexcept>

#include "anthropic_error_mapping.hpp"
#include "api_limits.hpp"
#include "policy.hpp"
#include "minimal/common/http_status.hpp"
#include "minimal/llm/policy.hpp"

namespace minimal::provider_anthropic {
namespace {

using nlohmann::json;


constexpr const char* kAnthropicVersion = "2023-06-01";

std::string ModelsUrl(const std::string& base_url, const std::optional<std::string>& after_id) {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v1/models?limit=" + std::to_string(kModelsPageLimit);
    if (after_id) url += "&after_id=" + *after_id;
    return url;
}

// A positive integer field, or nullopt. Non-positive and non-integer values
// are a misparse or a hostile entry (the provider-openai rule): storing one
// would make compaction fire every turn, or cap every request at nothing.
template <typename T>
std::optional<T> PositiveField(const json& entry, const char* field) {
    if (!entry.contains(field) || !entry.at(field).is_number_integer()) return std::nullopt;
    const std::int64_t value = entry.at(field).get<std::int64_t>();
    if (value <= 0 || value > std::numeric_limits<T>::max()) return std::nullopt;
    return static_cast<T>(value);
}

}  // namespace

ModelListPage ParseModelListPage(const std::string& body) {
    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::exception&) {
        throw std::runtime_error("model list: response is not valid JSON");
    }
    if (!doc.is_object() || !doc.contains("data") || !doc.at("data").is_array()) {
        throw std::runtime_error("model list: expected an object with a \"data\" array");
    }

    ModelListPage page;
    page.models.reserve(doc.at("data").size());
    for (auto& entry : doc.at("data")) {
        if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string() ||
            entry.at("id").get<std::string>().empty()) {
            continue;
        }
        llm::DiscoveredModel model;
        model.id = entry.at("id").get<std::string>();
        model.context_window_tokens = PositiveField<std::int64_t>(entry, "max_input_tokens");
        model.max_tokens = PositiveField<int>(entry, "max_tokens");
        json metadata = std::move(entry);
        metadata.erase("id");
        model.metadata = std::move(metadata);
        page.models.push_back(std::move(model));
    }

    const bool has_more = doc.contains("has_more") && doc.at("has_more").is_boolean() &&
                          doc.at("has_more").get<bool>();
    if (has_more && doc.contains("last_id") && doc.at("last_id").is_string() &&
        !doc.at("last_id").get<std::string>().empty()) {
        page.next_after_id = doc.at("last_id").get<std::string>();
    }
    return page;
}

std::vector<llm::DiscoveredModel> AnthropicModelCatalog::List(
    const llm::ProviderConfig& config) const {
    std::vector<llm::DiscoveredModel> out;
    std::optional<std::string> after_id;
    for (int page_number = 0; page_number < kMaxModelPages; ++page_number) {
        net::HttpRequest request;
        request.url = ModelsUrl(config.base_url, after_id);
        request.connect_timeout = config.connect_timeout;
        request.request_timeout = llm::kListModelsTimeout;
        request.headers.push_back({"anthropic-version", kAnthropicVersion});
        if (const auto key = llm::RequestApiKey(config)) {
            request.headers.push_back({"x-api-key", *key});
        }
        // Same headers the chat path sends (AnthropicProvider::BuildHttpRequest):
        // a header-auth gateway 401s a discovery call that carries only the key.
        for (const auto& [name, value] : config.headers) {
            request.headers.push_back({name, value});
        }

        const net::HttpResponse response = http_.Get(request);
        if (!common::IsSuccessStatus(response.status)) {
            throw ClassifyHttpFailure(response.status, response.body, response.headers);
        }
        ModelListPage page = ParseModelListPage(response.body);
        for (auto& model : page.models) out.push_back(std::move(model));
        if (!page.next_after_id || page.next_after_id == after_id) return out;
        after_id = std::move(page.next_after_id);
    }
    return out;
}

}  // namespace minimal::provider_anthropic
