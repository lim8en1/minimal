// Live model-catalog test (track N). Opt-in, because it needs a running
// OpenAI-shaped backend: set MINIMAL_LLM_BASE_URL (e.g.
// http://localhost:8000/v1 for a local llama.cpp) and the case exercises the
// REAL transport -- URL build, HTTP GET, parse -- against the actual backend.
// Unset (the default, and CI), it skips. Everything here is also covered
// offline by the loopback http_client tests and the canned ParseModelList
// bodies; this file exists because "the transport is covered" once was false.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

#include "minimal/llm/model_catalog.hpp"
#include "openai_model_catalog.hpp"

using minimal::llm::DiscoveredModel;
using minimal::provider_openai::OpenAiModelCatalog;

TEST_CASE("model discovery lists a live OpenAI-shaped backend", "[model_catalog][live]") {
    const char* base_url = std::getenv("MINIMAL_LLM_BASE_URL");
    if (base_url == nullptr || std::string(base_url).empty()) {
        SKIP("MINIMAL_LLM_BASE_URL not set -- no live backend to talk to");
    }

    OpenAiModelCatalog catalog{minimal::llm::HttpClient{}};
    minimal::llm::ProviderConfig config;
    config.base_url = base_url;

    const std::vector<DiscoveredModel> models = catalog.List(config);
    REQUIRE_FALSE(models.empty());
    for (const auto& model : models) {
        CHECK_FALSE(model.id.empty());
        // llama.cpp reports meta.n_ctx_train; the live server's window must
        // arrive as a real number, never a guess -- the offline degrade rule
        // (missing field -> nullopt) verified against a backend that has one.
        if (model.id == "ornith-1.5-9b") {
            REQUIRE(model.context_window_tokens.has_value());
            CHECK(*model.context_window_tokens == 262144);
        }
    }
}
