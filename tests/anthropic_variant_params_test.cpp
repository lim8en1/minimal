#include <catch2/catch_test_macros.hpp>

#include "anthropic_variant_params.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;

TEST_CASE("DeriveParameters: budget_tokens clamps against max_tokens", "[anthropic_variant]") {
    AnthropicVariantParams params;
    const auto [json, clamped] = params.DeriveParameters("low", 100000);
    CHECK_FALSE(clamped);  // 4096 is well under 100000 - 1024
    CHECK(json["thinking"]["budget_tokens"] == 4096);
}

TEST_CASE("DeriveParameters: a tight max_tokens clamps down, never negative",
          "[anthropic_variant]") {
    // Regression: `value = *max_tokens - 1024` had no floor, so max_tokens
    // <= 1024 sent a negative budget_tokens -- a 400 instead of the graceful
    // degradation the clamp exists to provide.
    AnthropicVariantParams params;
    const auto [json, clamped] = params.DeriveParameters("low", 3000);
    REQUIRE(clamped);
    const int budget = json["thinking"]["budget_tokens"].get<int>();
    CHECK(budget == 1976);  // 3000 - 1024
    CHECK(budget >= 1024);  // Anthropic's own minimum
}

TEST_CASE("DeriveParameters: max_tokens too small for thinking at all skips it",
          "[anthropic_variant]") {
    // max_tokens=500 can't hold both the 1024-token minimum budget AND 1024
    // reserved for visible output -- no valid budget_tokens exists, so
    // thinking is skipped rather than sending a guaranteed-invalid one.
    AnthropicVariantParams params;
    const auto [json, clamped] = params.DeriveParameters("low", 500);
    CHECK_FALSE(clamped);
    CHECK(json.empty());
    CHECK_FALSE(json.contains("thinking"));

    // Exactly at the boundary (2048 = 1024 + 1024): the minimum budget fits.
    const auto [at_boundary, boundary_clamped] = params.DeriveParameters("low", 2048);
    REQUIRE(boundary_clamped);
    CHECK(at_boundary["thinking"]["budget_tokens"] == 1024);

    // One below the boundary: no room, skip thinking.
    const auto [below, below_clamped] = params.DeriveParameters("low", 2047);
    CHECK_FALSE(below_clamped);
    CHECK(below.empty());
}

TEST_CASE("DeriveParameters: 'none' omits thinking without consulting max_tokens",
          "[anthropic_variant]") {
    AnthropicVariantParams params;
    const auto [json, clamped] = params.DeriveParameters("none", 500);
    CHECK_FALSE(clamped);
    CHECK(json.empty());
}

TEST_CASE("DeriveParameters: an unmapped effort word throws rather than sending garbage",
          "[anthropic_variant]") {
    AnthropicVariantParams params;
    CHECK_THROWS_AS(params.DeriveParameters("minimal", std::nullopt), std::runtime_error);
}

TEST_CASE("DeriveParameters: every rung of the ladder has its own budget",
          "[anthropic_variant]") {
    // Core passes the effort name verbatim, so @high, @xhigh and @max must not
    // collapse into one budget.
    AnthropicVariantParams params;
    CHECK(params.DeriveParameters("high", 200000).first["thinking"]["budget_tokens"] == 16384);
    CHECK(params.DeriveParameters("xhigh", 200000).first["thinking"]["budget_tokens"] == 32768);
    CHECK(params.DeriveParameters("max", 200000).first["thinking"]["budget_tokens"] == 65536);
}
