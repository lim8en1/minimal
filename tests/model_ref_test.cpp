// Pure-logic tier for src/common/model_ref.{hpp,cpp}: the two reference
// parsers and the split of responsibilities between them -- strict for
// authoring surfaces (providers.json, agents/*.md), first-slash for
// interactive surfaces (--model, /model, the picker).

#include <catch2/catch_test_macros.hpp>

#include "minimal/common/model_ref.hpp"

using minimal::common::ParseModelRef;
using minimal::common::SplitModelRef;

TEST_CASE("ParseModelRef: exactly one '/' separating two non-empty halves") {
    const auto ref = ParseModelRef("local/qwen4");
    REQUIRE(ref.has_value());
    CHECK(ref->provider == "local");
    CHECK(ref->model == "qwen4");
    CHECK(ref->str() == "local/qwen4");

    SECTION("the documented rejections hold") {
        CHECK_FALSE(ParseModelRef("qwen4").has_value());     // no provider
        CHECK_FALSE(ParseModelRef("local/").has_value());    // empty model
        CHECK_FALSE(ParseModelRef("/qwen4").has_value());    // empty provider
        CHECK_FALSE(ParseModelRef("a/b/c").has_value());     // ambiguous split
    }

    SECTION("surrounding whitespace is trimmed, not parsed into ids") {
        // A ref copied from a doc with a trailing space produced provider
        // " gpt-4o" that matched no catalog entry.
        const auto padded = ParseModelRef("  local/qwen4  ");
        REQUIRE(padded.has_value());
        CHECK(padded->provider == "local");
        CHECK(padded->model == "qwen4");
        const auto inner = SplitModelRef("local / qwen4");
        REQUIRE(inner.has_value());
        CHECK(inner->provider == "local");
        CHECK(inner->model == "qwen4");
    }
}

TEST_CASE("SplitModelRef: the first '/' splits, later segments stay the model",
          "[openrouter]") {
    // Discovered catalogs legitimately serve slashy ids: provider
    // "openrouter", model id "anthropic/claude-opus-5".
    const auto ref = SplitModelRef("openrouter/anthropic/claude-opus-5");
    REQUIRE(ref.has_value());
    CHECK(ref->provider == "openrouter");
    CHECK(ref->model == "anthropic/claude-opus-5");

    SECTION("the empty-half rejections match ParseModelRef") {
        CHECK_FALSE(SplitModelRef("qwen4").has_value());
        CHECK_FALSE(SplitModelRef("local/").has_value());
        CHECK_FALSE(SplitModelRef("/qwen4").has_value());
    }
}
