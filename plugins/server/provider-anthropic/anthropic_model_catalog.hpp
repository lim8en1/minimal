#pragma once

// Model catalog for the Anthropic Messages API (docs/EXTENSIBILITY.md §2.6):
// GET <base>/v1/models, paginated. Each entry reports its own limits and
// capabilities, so a `"discover": true` provider entry needs no per-model
// config at all:
//
//   max_input_tokens -> DiscoveredModel::context_window_tokens
//   max_tokens       -> DiscoveredModel::max_tokens (the output cap -- what
//                       anthropic_request.cpp otherwise demands by hand)
//   capabilities     -> metadata, verbatim: the effort ladder
//                       (llm::AdvertisedEfforts) and the thinking shape
//                       (AnthropicVariantParams) are read from it

#include <optional>
#include <string>
#include <vector>

#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/net/http_client.hpp"

namespace minimal::provider_anthropic {

// One page of a /v1/models listing.
struct ModelListPage {
    std::vector<llm::DiscoveredModel> models;
    // The `after_id` for the next page; nullopt when this was the last one.
    std::optional<std::string> next_after_id;
};

// Pure: parses one /v1/models response body. Throws std::runtime_error on a
// body that is not an object with a "data" array. An entry without a usable
// "id" is skipped, not fatal (the provider-openai posture).
ModelListPage ParseModelListPage(const std::string& body);

// The transport half. Stateless, like the provider: List() may be called
// concurrently from resolvers on several session workers and the command pool.
class AnthropicModelCatalog : public llm::ModelCatalog {
public:
    explicit AnthropicModelCatalog(const net::HttpClient& http) : http_(http) {}

    std::vector<llm::DiscoveredModel> List(const llm::ProviderConfig& config) const override;

private:
    net::HttpClient http_;
};

}  // namespace minimal::provider_anthropic
