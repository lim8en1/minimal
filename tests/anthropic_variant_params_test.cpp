#include <catch2/catch_test_macros.hpp>

#include "anthropic_variant_params.hpp"

using namespace minimal;
using namespace minimal::provider_anthropic;
using nlohmann::json;

namespace {

// /v1/models capability reports, trimmed to the branches the mapping reads.
// Opus-5.5-shaped: adaptive thinking only, the whole effort ladder.
const json kAdaptiveModel = json::parse(R"({"capabilities": {
    "thinking": {"supported": true, "types": {"enabled": {"supported": false},
                                               "adaptive": {"supported": true}}},
    "effort": {"supported": true, "low": {"supported": true}, "medium": {"supported": true},
               "high": {"supported": true}, "xhigh": {"supported": true},
               "max": {"supported": true}}}})");

// Haiku-4.5-shaped: budgeted thinking only, no effort parameter.
const json kBudgetModel = json::parse(R"({"capabilities": {
    "thinking": {"supported": true, "types": {"enabled": {"supported": true},
                                               "adaptive": {"supported": false}}},
    "effort": {"supported": false}}})");

}  // namespace

TEST_CASE("DeriveParameters: an adaptive model gets adaptive thinking and the effort word",
          "[anthropic_variant]") {
    AnthropicVariantParams params;
    const auto [out, clamped] = *params.DeriveParameters("high", 128000, kAdaptiveModel);
    CHECK_FALSE(clamped);
    CHECK(out["thinking"]["type"] == "adaptive");
    CHECK(out["thinking"]["display"] == "summarized");
    CHECK_FALSE(out["thinking"].contains("budget_tokens"));
    CHECK(out["output_config"]["effort"] == "high");
}

TEST_CASE("DeriveParameters: a model with no capability report is treated as current",
          "[anthropic_variant]") {
    // A static model discovery never saw: adaptive, the word verbatim -- never
    // budget_tokens, which every current model rejects.
    AnthropicVariantParams params;
    const auto [out, clamped] = *params.DeriveParameters("xhigh", 32000, json::object());
    CHECK_FALSE(clamped);
    CHECK(out["thinking"]["type"] == "adaptive");
    CHECK(out["output_config"]["effort"] == "xhigh");
}

TEST_CASE("DeriveParameters: an unsupported effort word steps down to the nearest lower rung",
          "[anthropic_variant]") {
    // Opus-4.6-shaped: no xhigh.
    json model = kAdaptiveModel;
    model["capabilities"]["effort"]["xhigh"]["supported"] = false;
    AnthropicVariantParams params;
    CHECK(params.DeriveParameters("xhigh", 128000, model)->first["output_config"]["effort"] ==
          "high");
    CHECK(params.DeriveParameters("max", 128000, model)->first["output_config"]["effort"] ==
          "max");

    // Nothing at or below the word: no effort field at all.
    model["capabilities"]["effort"]["low"]["supported"] = false;
    const auto [low, clamped] = *params.DeriveParameters("low", 128000, model);
    CHECK_FALSE(low.contains("output_config"));
    CHECK(low["thinking"]["type"] == "adaptive");
}

TEST_CASE("DeriveParameters: a budget-only model gets budget_tokens and no effort",
          "[anthropic_variant]") {
    AnthropicVariantParams params;
    const auto [out, clamped] = *params.DeriveParameters("low", 64000, kBudgetModel);
    CHECK_FALSE(clamped);
    CHECK(out["thinking"]["type"] == "enabled");
    CHECK(out["thinking"]["budget_tokens"] == 4096);
    CHECK_FALSE(out.contains("output_config"));
}

TEST_CASE("DeriveParameters: budget_tokens clamps against max_tokens, never negative",
          "[anthropic_variant]") {
    // Regression: `value = *max_tokens - 1024` had no floor, so max_tokens
    // <= 1024 sent a negative budget_tokens -- a 400 instead of the graceful
    // degradation the clamp exists to provide.
    AnthropicVariantParams params;
    const auto [out, clamped] = *params.DeriveParameters("low", 3000, kBudgetModel);
    REQUIRE(clamped);
    CHECK(out["thinking"]["budget_tokens"] == 1976);  // 3000 - 1024
}

TEST_CASE("DeriveParameters: max_tokens too small for a budget skips thinking",
          "[anthropic_variant]") {
    // 2047 can't hold both the 1024-token minimum budget AND 1024 reserved
    // for visible output; 2048 exactly can.
    AnthropicVariantParams params;
    const auto [below, below_clamped] = *params.DeriveParameters("low", 2047, kBudgetModel);
    CHECK_FALSE(below_clamped);
    CHECK_FALSE(below.contains("thinking"));
    const auto [at, at_clamped] = *params.DeriveParameters("low", 2048, kBudgetModel);
    REQUIRE(at_clamped);
    CHECK(at["thinking"]["budget_tokens"] == 1024);
}

TEST_CASE("DeriveParameters: every rung of a budget ladder has its own budget",
          "[anthropic_variant]") {
    AnthropicVariantParams params;
    CHECK(params.DeriveParameters("high", 200000, kBudgetModel)->first["thinking"]["budget_tokens"] ==
          16384);
    CHECK(params.DeriveParameters("xhigh", 200000, kBudgetModel)->first["thinking"]["budget_tokens"] ==
          32768);
    CHECK(params.DeriveParameters("max", 200000, kBudgetModel)->first["thinking"]["budget_tokens"] ==
          65536);
}

TEST_CASE("DeriveParameters: a model without thinking gets effort alone",
          "[anthropic_variant]") {
    const json model = json::parse(R"({"capabilities": {
        "thinking": {"supported": false},
        "effort": {"supported": true, "low": {"supported": true}, "medium": {"supported": true},
                   "high": {"supported": true}}}})");
    AnthropicVariantParams params;
    const auto [out, clamped] = *params.DeriveParameters("high", 64000, model);
    CHECK_FALSE(out.contains("thinking"));
    CHECK(out["output_config"]["effort"] == "high");
}

TEST_CASE("DeriveParameters: 'none' omits thinking on every model", "[anthropic_variant]") {
    AnthropicVariantParams params;
    CHECK(params.DeriveParameters("none", 500, kBudgetModel)->first.empty());
    CHECK(params.DeriveParameters("none", 128000, kAdaptiveModel)->first.empty());
}

TEST_CASE("DeriveParameters: an unmapped effort word throws rather than sending garbage",
          "[anthropic_variant]") {
    AnthropicVariantParams params;
    CHECK_THROWS_AS(params.DeriveParameters("minimal", std::nullopt, json::object()),
                    std::runtime_error);
}
