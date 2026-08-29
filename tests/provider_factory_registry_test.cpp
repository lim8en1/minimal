#include <catch2/catch_test_macros.hpp>

#include "minimal/llm/provider_factory_registry.hpp"

using namespace minimal;

namespace {

class StubProvider : public llm::LlmProvider {
public:
    explicit StubProvider(std::string model) : model_(std::move(model)) {}
    llm::ChatResponse Complete(const llm::ChatRequest&) override {
        llm::ChatResponse r;
        r.model = model_;
        return r;
    }
    llm::ChatResponse StreamComplete(const llm::ChatRequest& request,
                                      const llm::StreamEventSink&) override {
        return Complete(request);
    }
private:
    std::string model_;
};

llm::ProviderFactoryRegistry::Factory StubFactory() {
    return [](const llm::ProviderConfig& config) {
        return std::make_unique<StubProvider>(config.model_name);
    };
}

}  // namespace

TEST_CASE("a registered kind is constructible", "[provider_factory]") {
    llm::ProviderFactoryRegistry registry;
    CHECK(registry.Register("openai", StubFactory()));
    CHECK(registry.Has("openai"));

    llm::ProviderConfig config;
    config.model_name = "gpt-test";
    auto provider = registry.Create("openai", config);
    REQUIRE(provider != nullptr);
    CHECK(provider->Complete({}).model == "gpt-test");
}

TEST_CASE("an unknown kind yields nullptr rather than throwing", "[provider_factory]") {
    // A providers.json entry naming a kind no plugin supplied is a startup
    // sanity-check failure, not a crash.
    llm::ProviderFactoryRegistry registry;
    CHECK_FALSE(registry.Has("anthropic"));
    CHECK(registry.Create("anthropic", {}) == nullptr);
}

TEST_CASE("a duplicate kind is reported rather than silently overwriting",
          "[provider_factory]") {
    // The loader needs to name both colliding plugins, so registration reports
    // failure instead of throwing from inside a plugin's callback.
    llm::ProviderFactoryRegistry registry;
    CHECK(registry.Register("openai", StubFactory()));
    CHECK_FALSE(registry.Register("openai", StubFactory()));
    CHECK(registry.kinds().size() == 1);
}

TEST_CASE("kinds() is sorted for deterministic diagnostics", "[provider_factory]") {
    llm::ProviderFactoryRegistry registry;
    registry.Register("zeta", StubFactory());
    registry.Register("alpha", StubFactory());
    registry.Register("mid", StubFactory());

    CHECK(registry.kinds() == std::vector<std::string>{"alpha", "mid", "zeta"});
}

TEST_CASE("a null factory is rejected", "[provider_factory]") {
    llm::ProviderFactoryRegistry registry;
    CHECK_FALSE(registry.Register("broken", nullptr));
    CHECK_FALSE(registry.Has("broken"));
}
