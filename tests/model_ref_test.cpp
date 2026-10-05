// Pure-logic tier for src/common/model_ref.{hpp,cpp}: `<provider>/<model>`
// parsing, split at the first slash.

#include <catch2/catch_test_macros.hpp>

#include "minimal/common/model_ref.hpp"

using minimal::common::SplitModelRef;

TEST_CASE("SplitModelRef trims surrounding whitespace on both halves") {
    // A ref copied from a doc with a trailing space produced provider
    // " gpt-4o" that matched no catalog entry.
    const auto padded = SplitModelRef("  local/qwen4  ");
    REQUIRE(padded.has_value());
    CHECK(padded->provider == "local");
    CHECK(padded->model == "qwen4");
    CHECK(padded->str() == "local/qwen4");
    const auto inner = SplitModelRef("local / qwen4");
    REQUIRE(inner.has_value());
    CHECK(inner->provider == "local");
    CHECK(inner->model == "qwen4");
}

TEST_CASE("SplitModelRef: the first '/' splits, later segments stay the model",
          "[openrouter]") {
    // Discovered catalogs legitimately serve slashy ids: provider
    // "openrouter", model id "anthropic/claude-opus-5".
    const auto ref = SplitModelRef("openrouter/anthropic/claude-opus-5");
    REQUIRE(ref.has_value());
    CHECK(ref->provider == "openrouter");
    CHECK(ref->model == "anthropic/claude-opus-5");

    SECTION("an empty half is rejected") {
        CHECK_FALSE(SplitModelRef("qwen4").has_value());
        CHECK_FALSE(SplitModelRef("local/").has_value());
        CHECK_FALSE(SplitModelRef("/qwen4").has_value());
        CHECK_FALSE(SplitModelRef("").has_value());
        CHECK_FALSE(SplitModelRef("/").has_value());
    }
}
