// The cost-view status segment's pure formatters (extracted from the plugin
// TU so they are testable -- the logo_generator pattern).

#include "cost_view_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using minimal::client::cost_view::FormatCost;
using minimal::client::cost_view::FormatTokens;
using minimal::client::cost_view::FormatTotals;

TEST_CASE("FormatTokens tiers at k/M/G and never truncates at extremes") {
    CHECK(FormatTokens(123) == "123");
    CHECK(FormatTokens(12'345) == "12.3k");
    CHECK(FormatTokens(1'234'567) == "1.2M");
    // The G tier: the old table stopped at M, so a billion-token session
    // rendered "1234.5M".
    CHECK(FormatTokens(1'234'567'890) == "1.2G");
}

TEST_CASE("FormatCost keeps sub-cent precision") {
    CHECK(FormatCost(0.0) == "$0.0000");
    CHECK(FormatCost(0.004) == "$0.0040");
    CHECK(FormatCost(1.5) == "$1.50");
}

TEST_CASE("unknown or anomalous cost renders as cost?, never as $0.0000") {
    // total_cost_known DEFAULTED to true: a blob missing the field (or
    // written before it existed) rendered "$0.0000" -- free. The default is
    // now unknown.
    CHECK(FormatTotals(nlohmann::json{{"turns", 3}}).value_or("") == "cost? · 0/0 tok");
    // An explicit false keeps the marker.
    CHECK(FormatTotals(nlohmann::json{{"turns", 3}, {"total_cost_known", false}})
              .value_or("") == "cost? · 0/0 tok");
    // A NEGATIVE total is corruption, not a refund: same marker.
    CHECK(FormatTotals(nlohmann::json{{"turns", 3},
                                      {"total_cost_known", true},
                                      {"total_cost", -1.5}})
              .value_or("") == "cost? · 0/0 tok");
}

TEST_CASE("known totals format normally") {
    const nlohmann::json totals{{"turns", 3},
                                {"total_cost_known", true},
                                {"total_cost", 0.42},
                                {"models", nlohmann::json::array({
                                     nlohmann::json{{"input_tokens", 1'234'567},
                                                    {"output_tokens", 12'345}},
                                 })}};
    CHECK(FormatTotals(totals).value_or("") == "$0.42 · 1.2M/12.3k tok");
}

TEST_CASE("a fresh session with no turns shows nothing, not $0.00") {
    CHECK(FormatTotals(nlohmann::json{{"turns", 0}, {"total_cost_known", true},
                                      {"total_cost", 0.0}}) == std::nullopt);
}
