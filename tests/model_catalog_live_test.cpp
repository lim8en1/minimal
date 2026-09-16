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

    OpenAiModelCatalog catalog{minimal::net::HttpClient{}};
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

// The autodetection's ground truth: OpenRouter's live /models payload, fed
// through the REAL ParseModelList into the REAL AdvertisedEfforts. Opt-in like
// the case above (unset env -> skip) because it needs internet; the offline
// cases pin the same machinery against canned bodies. glm-5.3-flash is the
// sentinel the feature was built from: its reasoning.supported_efforts must
// arrive as the advertised ladder, not empty and not a guess.
TEST_CASE("live OpenRouter catalog advertises glm-5.3-flash's effort ladder",
          "[model_catalog][live]") {
    const char* base_url = std::getenv("MINIMAL_LLM_BASE_URL");
    const char* openrouter = std::getenv("MINIMAL_LIVE_OPENROUTER");
    const bool live = (base_url != nullptr && std::string(base_url).find("openrouter") !=
                                                     std::string::npos) ||
                      (openrouter != nullptr && std::string(openrouter) == "1");
    if (!live) {
        SKIP("set MINIMAL_LIVE_OPENROUTER=1 -- no live OpenRouter to talk to");
    }

    OpenAiModelCatalog catalog{minimal::net::HttpClient{}};
    minimal::llm::ProviderConfig config;
    config.base_url = "https://openrouter.ai/api/v1";

    const std::vector<DiscoveredModel> models = catalog.List(config);
    const auto flash = std::find_if(models.begin(), models.end(),
                                    [](const DiscoveredModel& m) {
                                        return m.id == "z-ai/glm-5.3-flash";
                                    });
    REQUIRE(flash != models.end());
    // The ladder, through the real transport: AdvertisedEfforts reads the
    // metadata ParseModelList stored. Order follows the catalog verbatim;
    // membership (which rungs) is OpenRouter's to change -- a drift here is a
    // real-world change to reflect in the offline fixtures, not a code bug.
    const auto efforts = minimal::llm::AdvertisedEfforts(flash->metadata);
    REQUIRE_FALSE(efforts.empty());
    const bool has_high = std::find(efforts.begin(), efforts.end(), "high") != efforts.end();
    CHECK(has_high);
    INFO("live ladder for z-ai/glm-5.3-flash: " << efforts.size() << " efforts");
    for (const auto& effort : efforts) {
        INFO("  " << effort);
    }
    // The model also carries an output cap through the same payload.
    REQUIRE(flash->max_tokens.has_value());
    CHECK(*flash->max_tokens > 0);
}
