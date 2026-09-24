#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "minimal/llm/metadata_catalog.hpp"
#include "minimal/plugins/attributed_registry.hpp"

using minimal::plugins::AttributedRegistry;

TEST_CASE("AttributedRegistry: first registration wins and the loser is named",
          "[attributed_registry]") {
    AttributedRegistry<int> registry;
    registry.SetAttribution("alpha");
    REQUIRE(registry.Register("kind", 1));
    registry.SetAttribution("beta");
    CHECK_FALSE(registry.Register("kind", 2));
    registry.ClearAttribution();

    REQUIRE(registry.FindEntry("kind") != nullptr);
    CHECK(*registry.FindEntry("kind") == 1);
    REQUIRE(registry.collisions().size() == 1);
    CHECK(registry.collisions()[0].kind == "kind");
    CHECK(registry.collisions()[0].first_plugin == "alpha");
    CHECK(registry.collisions()[0].second_plugin == "beta");
}

TEST_CASE("AttributedRegistry: ownership queries and removal are per plugin",
          "[attributed_registry]") {
    AttributedRegistry<int> registry;
    REQUIRE(registry.Register("core-kind", 0));  // no attribution: core
    registry.SetAttribution("alpha");
    REQUIRE(registry.Register("b", 1));
    REQUIRE(registry.Register("a", 2));
    registry.SetAttribution("beta");
    REQUIRE(registry.Register("c", 3));
    registry.ClearAttribution();

    CHECK(registry.KeysOwnedBy("alpha") == std::vector<std::string>{"a", "b"});
    CHECK(registry.KeysOwnedBy("") == std::vector<std::string>{"core-kind"});
    CHECK(registry.kinds() == std::vector<std::string>{"a", "b", "c", "core-kind"});

    registry.RemoveByAttribution("alpha");
    CHECK(registry.kinds() == std::vector<std::string>{"c", "core-kind"});
    CHECK_FALSE(registry.Has("a"));
}

namespace {
class NullCatalog : public minimal::llm::MetadataCatalog {
public:
    std::optional<minimal::llm::DiscoveredModel> Lookup(const minimal::llm::MetadataQuery&,
                                                        minimal::llm::MetadataMatch) const override {
        return std::nullopt;
    }
};
}  // namespace

TEST_CASE("MetadataCatalogRegistry: removal strips a plugin's fallbacks too",
          "[attributed_registry]") {
    minimal::llm::MetadataCatalogRegistry registry;
    registry.SetAttribution("alpha");
    REQUIRE(registry.RegisterFallback(std::make_shared<NullCatalog>()));
    REQUIRE(registry.Register("openai", std::make_shared<NullCatalog>()));
    registry.SetAttribution("beta");
    REQUIRE(registry.RegisterFallback(std::make_shared<NullCatalog>()));
    registry.ClearAttribution();

    CHECK(registry.HasFallbackFrom("alpha"));
    registry.RemoveByAttribution("alpha");
    CHECK_FALSE(registry.HasFallbackFrom("alpha"));
    CHECK(registry.HasFallbackFrom("beta"));
    CHECK(registry.fallbacks().size() == 1);
    CHECK_FALSE(registry.Has("openai"));
}
