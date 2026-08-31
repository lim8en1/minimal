#include <catch2/catch_test_macros.hpp>

#include "fakes.hpp"
#include "minimal/agent/provider_instance_cache.hpp"

using namespace minimal;

namespace {
// Counts constructions: the whole point of the cache is that this stays low.
class CountingFactory : public llm::LlmProvider {
public:
    llm::ChatResponse Complete(const llm::ChatRequest&) override { return {}; }
    llm::ChatResponse StreamComplete(const llm::ChatRequest&,
                                      const llm::StreamEventSink&) override {
        return {};
    }
    int constructed = 0;
};
}  // namespace

TEST_CASE("two sessions on one providers.json entry share one instance",
          "[provider_instance_cache]") {
    llm::ProviderFactoryRegistry factories;
    auto counting = std::make_shared<CountingFactory>();
    factories.Register("counting", [&](const llm::ProviderConfig&) {
        ++counting->constructed;
        return std::make_unique<CountingFactory>();
    });

    agent::ProviderInstanceCache cache(factories, common::DefaultLogger());
    const llm::ProviderConfig config;

    const auto a = cache.GetOrCreate("local", "counting", config);
    const auto b = cache.GetOrCreate("local", "counting", config);

    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a.get() == b.get());  // the SAME instance, not two lookalikes
    CHECK(counting->constructed == 1);
    CHECK(a.use_count() == 3);  // cache + two callers
}

TEST_CASE("different provider entries get different instances", "[provider_instance_cache]") {
    // Two entries may serve the same backend with different limits; the entry
    // name is what distinguishes them, so sharing across names would be wrong.
    llm::ProviderFactoryRegistry factories;
    int constructed = 0;
    factories.Register("counting", [&](const llm::ProviderConfig&) {
        ++constructed;
        return std::make_unique<CountingFactory>();
    });

    agent::ProviderInstanceCache cache(factories, common::DefaultLogger());
    const llm::ProviderConfig config;

    const auto a = cache.GetOrCreate("local", "counting", config);
    const auto b = cache.GetOrCreate("remote", "counting", config);
    CHECK(a.get() != b.get());
    CHECK(constructed == 2);
}

TEST_CASE("a failing construct is not cached", "[provider_instance_cache]") {
    // Returning nullptr reports "no provider" to the caller; caching the
    // failure would make every later session fail without even trying -- but
    // nothing here can be fixed at runtime today, so the value is in matching
    // the factory registry's contract, not in retry.
    llm::ProviderFactoryRegistry factories;
    factories.Register("counting", [](const llm::ProviderConfig&) {
        return std::unique_ptr<llm::LlmProvider>(nullptr);
    });

    agent::ProviderInstanceCache cache(factories, common::DefaultLogger());
    const llm::ProviderConfig config;

    CHECK(cache.GetOrCreate("local", "counting", config) == nullptr);
    CHECK(cache.GetOrCreate("local", "counting", config) == nullptr);
}
