// Model catalog tests (track N): the registry rules, and ParseModelList
// against canned bodies in the shapes the three covered backends actually
// serve. The transport half of OpenAiModelCatalog is one HttpClient GET --
// framed by the loopback http_client tests, and exercised end to end (real
// backend, real URL build, real parse) by model_catalog_live_test.cpp when
// MINIMAL_LLM_BASE_URL points at a running OpenAI-shaped server.

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "minimal/llm/model_catalog.hpp"
#include "openai_model_catalog.hpp"

using minimal::llm::DiscoveredModel;
using minimal::llm::ModelCatalogRegistry;
using minimal::provider_openai::ParseModelList;

// --- The registry ------------------------------------------------------------

namespace {

struct FakeCatalog : minimal::llm::ModelCatalog {
    std::vector<DiscoveredModel> List(const minimal::llm::ProviderConfig&) const override {
        return {{"fake", 1234, nlohmann::json::object(), std::nullopt}};
    }
};

}  // namespace

TEST_CASE("ModelCatalogRegistry keys on the provider kind string", "[model_catalog]") {
    ModelCatalogRegistry registry;
    auto catalog = std::make_shared<FakeCatalog>();

    CHECK(registry.Find("openai") == nullptr);  // no catalog registered yet
    CHECK_FALSE(registry.Has("openai"));
    CHECK(registry.kinds().empty());

    CHECK(registry.Register("openai", catalog));
    CHECK(registry.Find("openai") == catalog.get());
    CHECK(registry.Has("openai"));

    // Same kind string as the provider factory registry: one `backend` value
    // selects both how to chat and how to list.
    CHECK_FALSE(registry.Register("openai", catalog));  // collision refused
    CHECK(registry.Find("openai") == catalog.get());    // first wins

    // Sorted, so diagnostics are deterministic.
    CHECK(registry.Register("anthropic", catalog));
    CHECK(registry.kinds() == std::vector<std::string>{"anthropic", "openai"});

    // A null catalog is a registration that did not happen.
    CHECK_FALSE(registry.Register("null", nullptr));
}

// --- ParseModelList: OpenRouter shape ----------------------------------------

TEST_CASE("ParseModelList reads OpenRouter's context_length and keeps metadata",
          "[model_catalog]") {
    const auto models = ParseModelList(R"({"data":[
        {"id": "deepseek/deepseek-r1", "context_length": 163840,
         "top_provider": {"max_completion_tokens": 65536, "context_length": 163840,
                          "is_moderated": true},
         "pricing": {"prompt": "0.0000005"}, "quantization": "fp8"}
    ]})");

    REQUIRE(models.size() == 1);
    CHECK(models[0].id == "deepseek/deepseek-r1");
    REQUIRE(models[0].context_window_tokens.has_value());
    CHECK(*models[0].context_window_tokens == 163840);
    // The backend's own output cap, where it reports one.
    REQUIRE(models[0].max_tokens.has_value());
    CHECK(*models[0].max_tokens == 65536);
    // Opaque to core, free for a future consumer: everything but the id.
    CHECK(models[0].metadata.at("pricing").at("prompt") == "0.0000005");
    CHECK(models[0].metadata.at("quantization") == "fp8");
    CHECK_FALSE(models[0].metadata.contains("id"));
}

TEST_CASE("ParseModelList degrades a missing output cap to nullopt", "[model_catalog]") {
    // top_provider is OpenRouter-shaped; a plain OpenAI-shape /models body (and
    // an OpenRouter entry without the field, or with a non-positive one) must
    // yield no cap rather than a fabricated or poisoned one.
    const auto models = ParseModelList(R"({"data":[
        {"id": "no-top-provider", "context_length": 8192},
        {"id": "empty-top-provider", "top_provider": {}, "context_length": 8192},
        {"id": "no-cap-field", "top_provider": {"is_moderated": false}},
        {"id": "garbage-cap", "top_provider": {"max_completion_tokens": "lots"}},
        {"id": "zero-cap", "top_provider": {"max_completion_tokens": 0}},
        {"id": "negative-cap", "top_provider": {"max_completion_tokens": -5}}
    ]})");

    REQUIRE(models.size() == 6);
    for (const auto& model : models) {
        CHECK_FALSE(model.max_tokens.has_value());
    }
}

// --- ParseModelList: llama.cpp shape -----------------------------------------

TEST_CASE("ParseModelList reads llama.cpp's meta.n_ctx_train", "[model_catalog]") {
    const auto models = ParseModelList(R"({"data":[
        {"id": "qwen2.5-coder-32b-instruct",
         "meta": {"n_ctx_train": 32768, "n_params": 32760256128}}
    ]})");

    REQUIRE(models.size() == 1);
    CHECK(models[0].id == "qwen2.5-coder-32b-instruct");
    REQUIRE(models[0].context_window_tokens.has_value());
    CHECK(*models[0].context_window_tokens == 32768);
    CHECK(models[0].metadata.at("meta").at("n_params") == 32760256128);
}

// --- ParseModelList: vLLM shape ----------------------------------------------

TEST_CASE("ParseModelList reads vLLM's max_model_len", "[model_catalog]") {
    const auto models = ParseModelList(R"({"data":[
        {"id": "Qwen/Qwen3-32B", "max_model_len": 40960, "object": "model"}
    ]})");

    REQUIRE(models.size() == 1);
    CHECK(models[0].id == "Qwen/Qwen3-32B");
    REQUIRE(models[0].context_window_tokens.has_value());
    CHECK(*models[0].context_window_tokens == 40960);
}

// --- ParseModelList: degradation and refusal ---------------------------------

TEST_CASE("ParseModelList degrades a missing window to nullopt, never a guess",
          "[model_catalog][invariant]") {
    // A model without any window field must not fabricate one: the alternative
    // is compaction firing against a made-up number.
    const auto models = ParseModelList(R"({"data":[
        {"id": "bare"},
        {"id": "bad-window", "context_length": "unknown"},
        {"id": "null-window", "context_length": null}
    ]})");

    REQUIRE(models.size() == 3);
    for (const auto& model : models) {
        CHECK_FALSE(model.context_window_tokens.has_value());
    }
}

TEST_CASE("ParseModelList keeps a windowed model beside an unwindowed one",
          "[model_catalog]") {
    const auto models = ParseModelList(R"({"data":[
        {"id": "no-window"},
        {"id": "windowed", "context_length": 8192}
    ]})");

    REQUIRE(models.size() == 2);
    CHECK_FALSE(models[0].context_window_tokens.has_value());
    CHECK(*models[1].context_window_tokens == 8192);
}

TEST_CASE("ParseModelList skips entries without a usable id", "[model_catalog]") {
    // One malformed row must not take every good model down with it -- but an
    // id-less entry cannot be addressed as <provider>/<model>, so it is skipped
    // rather than served.
    const auto models = ParseModelList(R"({"data":[
        {"context_length": 4096},
        {"id": "", "context_length": 4096},
        {"id": 42},
        {},
        {"id": "good", "context_length": 4096}
    ]})");

    REQUIRE(models.size() == 1);
    CHECK(models[0].id == "good");
}

TEST_CASE("ParseModelList refuses bodies that are not a model list", "[model_catalog]") {
    // Refusal, not a partial list served as complete: the caller negative-caches
    // the failure, and a silent empty list would read as "backend serves
    // nothing".
    CHECK_THROWS_AS(ParseModelList("not json at all"), std::runtime_error);
    CHECK_THROWS_AS(ParseModelList(R"([{"id": "x"}])"), std::runtime_error);
    CHECK_THROWS_AS(ParseModelList(R"({"object": "list"})"), std::runtime_error);
    CHECK_THROWS_AS(ParseModelList(R"({"data": {"id": "x"}})"), std::runtime_error);
}
