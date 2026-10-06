// provider-anthropic's model catalog: parsing a /v1/models page, and the
// paginating transport against a real loopback server.

#include <catch2/catch_test_macros.hpp>

#include "anthropic_model_catalog.hpp"
#include "loopback_http_server.hpp"
#include "minimal/llm/model_catalog.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;
using minimal::test::HttpResponseOf;
using minimal::test::LoopbackHttpServer;

namespace {

// Two models in the live /v1/models shape, trimmed.
constexpr const char* kPageOne = R"({
  "data": [
    {"type": "model", "id": "claude-opus-5-5", "display_name": "Claude Opus 5.5",
     "created_at": "2026-09-01T00:00:00Z",
     "max_input_tokens": 1000000, "max_tokens": 128000,
     "capabilities": {
       "thinking": {"supported": true, "types": {"enabled": {"supported": false},
                                                  "adaptive": {"supported": true}}},
       "effort": {"supported": true, "low": {"supported": true}, "medium": {"supported": true},
                  "high": {"supported": true}, "xhigh": {"supported": true},
                  "max": {"supported": true}}}},
    {"type": "model", "id": "claude-haiku-4-5", "display_name": "Claude Haiku 4.5",
     "max_input_tokens": 200000, "max_tokens": 64000,
     "capabilities": {
       "thinking": {"supported": true, "types": {"enabled": {"supported": true},
                                                  "adaptive": {"supported": false}}},
       "effort": {"supported": false}}}
  ],
  "has_more": true, "first_id": "claude-opus-5-5", "last_id": "claude-haiku-4-5"
})";

constexpr const char* kPageTwo = R"({
  "data": [{"type": "model", "id": "claude-sonnet-5-5", "max_input_tokens": 1000000,
            "max_tokens": 128000}],
  "has_more": false, "first_id": "claude-sonnet-5-5", "last_id": "claude-sonnet-5-5"
})";

llm::ProviderConfig ConfigFor(const LoopbackHttpServer& server) {
    llm::ProviderConfig config;
    config.base_url = server.url("");  // no /v1: the catalog appends /v1/models
    return config;
}

}  // namespace

TEST_CASE("ParseModelListPage: limits and capabilities map onto DiscoveredModel",
          "[anthropic_catalog]") {
    const ModelListPage page = ParseModelListPage(kPageOne);
    REQUIRE(page.models.size() == 2);

    const auto& opus = page.models[0];
    CHECK(opus.id == "claude-opus-5-5");
    CHECK(opus.context_window_tokens == 1000000);
    CHECK(opus.max_tokens == 128000);
    CHECK(opus.metadata["capabilities"]["thinking"]["types"]["adaptive"]["supported"] == true);
    CHECK_FALSE(opus.metadata.contains("id"));  // the id is the model, not metadata

    // The effort ladder core autodetects from the same metadata.
    const auto efforts = llm::AdvertisedEfforts(opus.metadata);
    REQUIRE(efforts.size() == 5);
    CHECK(efforts.front() == "low");
    CHECK(efforts.back() == "max");
    CHECK(llm::AdvertisedEfforts(page.models[1].metadata).empty());  // no effort parameter

    REQUIRE(page.next_after_id.has_value());
    CHECK(*page.next_after_id == "claude-haiku-4-5");
}

TEST_CASE("ParseModelListPage: the last page names no cursor", "[anthropic_catalog]") {
    const ModelListPage page = ParseModelListPage(kPageTwo);
    REQUIRE(page.models.size() == 1);
    CHECK_FALSE(page.next_after_id.has_value());
}

TEST_CASE("ParseModelListPage: bad limits are absent, bad rows skipped, bad bodies refused",
          "[anthropic_catalog]") {
    const ModelListPage page = ParseModelListPage(R"({"data": [
        {"id": "neg", "max_input_tokens": -1, "max_tokens": 0},
        {"id": "frac", "max_input_tokens": 1000.5, "max_tokens": "64000"},
        {"display_name": "no id"},
        "not an object"
    ]})");
    REQUIRE(page.models.size() == 2);
    for (const auto& model : page.models) {
        CHECK_FALSE(model.context_window_tokens.has_value());
        CHECK_FALSE(model.max_tokens.has_value());
    }
    CHECK_FALSE(page.next_after_id.has_value());

    CHECK_THROWS_AS(ParseModelListPage("not json"), std::runtime_error);
    CHECK_THROWS_AS(ParseModelListPage(R"({"models": []})"), std::runtime_error);
}

TEST_CASE("AnthropicModelCatalog: follows the cursor across pages, with Anthropic's headers",
          "[anthropic_catalog]") {
    LoopbackHttpServer server({HttpResponseOf(200, "OK", "application/json", kPageOne),
                               HttpResponseOf(200, "OK", "application/json", kPageTwo)});
    llm::ProviderConfig config = ConfigFor(server);
    config.headers.push_back({"x-gateway-tenant", "t1"});

    const auto models = AnthropicModelCatalog(net::HttpClient{}).List(config);
    REQUIRE(models.size() == 3);
    CHECK(models[2].id == "claude-sonnet-5-5");

    const auto requests = server.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].find("GET /v1/models?limit=1000 ") != std::string::npos);
    CHECK(requests[1].find("GET /v1/models?limit=1000&after_id=claude-haiku-4-5 ") !=
          std::string::npos);
    CHECK(requests[0].find("anthropic-version: 2023-06-01") != std::string::npos);
    CHECK(requests[0].find("x-gateway-tenant: t1") != std::string::npos);
}

TEST_CASE("AnthropicModelCatalog: a non-2xx answer throws, classified", "[anthropic_catalog]") {
    LoopbackHttpServer server({HttpResponseOf(
        401, "Unauthorized", "application/json",
        R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})")});
    CHECK_THROWS_AS(AnthropicModelCatalog(net::HttpClient{}).List(ConfigFor(server)),
                    llm::LlmError);
}

TEST_CASE("AnthropicModelCatalog: a cursor that repeats ends the listing", "[anthropic_catalog]") {
    // A backend whose has_more never turns false must not loop forever.
    constexpr const char* kStuck = R"({"data": [{"id": "m"}], "has_more": true, "last_id": "m"})";
    LoopbackHttpServer server({HttpResponseOf(200, "OK", "application/json", kStuck),
                               HttpResponseOf(200, "OK", "application/json", kStuck)});
    const auto models = AnthropicModelCatalog(net::HttpClient{}).List(ConfigFor(server));
    CHECK(models.size() == 2);
    CHECK(server.requests().size() == 2);
}
