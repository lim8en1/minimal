// The cost-view status segment's pure formatters (extracted from the plugin
// TU so they are testable -- the logo_generator pattern).

#include "cost_view_format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using minimal::client::cost_view::FormatCost;
using minimal::client::cost_view::FormatTokenDetail;
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

TEST_CASE("a partially-known total (mixed priced/unpriced models) shows a floor, not cost?") {
    // The local -> cloud mid-session model switch: the cloud model's turns
    // priced fine, the local model's did not, so total_cost_known is false
    // but total_cost is a real, non-zero partial sum. Discarding it (the old
    // two-way rule) hid real spend; the fix mirrors CostCell's ">= $X" floor.
    const nlohmann::json totals{{"turns", 5},
                                {"total_cost_known", false},
                                {"total_cost", 0.42},
                                {"unpriced_turns", 2},
                                {"models", nlohmann::json::array({
                                     nlohmann::json{{"input_tokens", 10'000},
                                                    {"output_tokens", 7'000}},
                                 })}};
    CHECK(FormatTotals(totals).value_or("") == ">= $0.42 · 10.0k/7.0k tok (+unpriced)");
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

TEST_CASE("FormatTokenDetail renders the four sums with i/o/cr/cw labels") {
    // An Anthropic-shaped session: all four fields reported, cumulative
    // across models.
    const nlohmann::json totals{{"turns", 3},
                                {"models", nlohmann::json::array({
                                     nlohmann::json{{"input_tokens", 3'100},
                                                    {"output_tokens", 812},
                                                    {"cache_read_tokens", 34'000},
                                                    {"cache_write_tokens", 240}},
                                 })}};
    CHECK(FormatTokenDetail(totals).value_or("") ==
          "i 3.1k · o 812 · cr 34.0k · cw 240");
}

TEST_CASE("FormatTokenDetail aggregates across models") {
    // A session that switched models mid-way: the totals payload carries one
    // entry PER MODEL, so the breakdown must sum -- the /cost table's
    // "total" row made the same choice.
    const nlohmann::json totals{{"turns", 5},
                                {"models", nlohmann::json::array({
                                     nlohmann::json{{"input_tokens", 1'000},
                                                    {"output_tokens", 100},
                                                    {"cache_read_tokens", 10'000},
                                                    {"cache_write_tokens", 200}},
                                     nlohmann::json{{"input_tokens", 2'000},
                                                    {"output_tokens", 50},
                                                    {"cache_read_tokens", 5'000},
                                                    {"cache_write_tokens", 0}},
                                 })}};
    CHECK(FormatTokenDetail(totals).value_or("") ==
          "i 3.0k · o 150 · cr 15.0k · cw 200");
}

TEST_CASE("FormatTokenDetail suppresses zero fields (OpenAI never reports cw)") {
    // OpenAI-shaped backends have no cache-write concept; "cw 0" is noise,
    // and so is "cr 0" for a backend that reported nothing cached at all.
    const nlohmann::json totals{{"turns", 2},
                                {"models", nlohmann::json::array({
                                     nlohmann::json{{"input_tokens", 45'100},
                                                    {"output_tokens", 3'200},
                                                    {"cache_read_tokens", 0},
                                                    {"cache_write_tokens", 0}},
                                 })}};
    CHECK(FormatTokenDetail(totals).value_or("") == "i 45.1k · o 3.2k");
}

TEST_CASE("FormatTokenDetail renders nothing for an all-zero or malformed payload") {
    // All four sums zero (nothing reported at all): the status bar's
    // unknown-segment rule -- nothing, never "i 0 · o 0".
    CHECK(FormatTokenDetail(nlohmann::json{{"turns", 4},
                                           {"models", nlohmann::json::array({
                                                nlohmann::json{{"input_tokens", 0},
                                                               {"output_tokens", 0},
                                                               {"cache_read_tokens", 0},
                                                               {"cache_write_tokens", 0}},
                                            })}}) == std::nullopt);
    // No models array at all.
    CHECK(FormatTokenDetail(nlohmann::json{{"turns", 4}}) == std::nullopt);
    // A non-object payload: the same malformed-input contract as FormatTotals.
    CHECK(FormatTokenDetail(nlohmann::json::array({1, 2})) == std::nullopt);
    // A negative count is corruption, not a refund: suppressed like zero.
    CHECK(FormatTokenDetail(nlohmann::json{{"models", nlohmann::json::array({
                                               nlohmann::json{{"input_tokens", -5},
                                                              {"output_tokens", 10}},
                                           })}}).value_or("") == "o 10");
}

// Regression: these formatters run unguarded inside a background poller's
// std::thread entry function (cost_view_plugin.cpp's RunLoop) -- an
// exception escaping there is std::terminate(), not a caught error. The
// totals payload arrives over PluginCall from another plugin's RPC reply, a
// shape this process does not control, so a field present with the WRONG
// type (an int where a bool was expected, a null entry in "models") must
// degrade the segment, never throw. nlohmann's own `.value(key, fallback)`
// throws type_error.302/.306 for exactly these shapes.
TEST_CASE("FormatTotals degrades instead of throwing on a wrong-typed field") {
    // total_cost_known present as an int, not a bool.
    CHECK_NOTHROW(FormatTotals(nlohmann::json{{"turns", 3}, {"total_cost_known", 1}}));
    CHECK(FormatTotals(nlohmann::json{{"turns", 3}, {"total_cost_known", 1}}).value_or("") ==
          "cost? · 0/0 tok");
    // total_cost present as a string.
    CHECK_NOTHROW(FormatTotals(
        nlohmann::json{{"turns", 3}, {"total_cost_known", true}, {"total_cost", "oops"}}));
    // turns present as a string.
    CHECK_NOTHROW(FormatTotals(nlohmann::json{{"turns", "oops"}}));
    CHECK(FormatTotals(nlohmann::json{{"turns", "oops"}}) == std::nullopt);
    // A non-object entry inside "models" (e.g. a malformed row is null).
    CHECK_NOTHROW(FormatTotals(nlohmann::json{
        {"turns", 3}, {"models", nlohmann::json::array({nullptr})}}));
    // A model row whose input_tokens is the wrong type.
    CHECK_NOTHROW(FormatTotals(nlohmann::json{
        {"turns", 3},
        {"models", nlohmann::json::array({nlohmann::json{{"input_tokens", "oops"}}})}}));
}

TEST_CASE("FormatTokenDetail degrades instead of throwing on a wrong-typed field") {
    CHECK_NOTHROW(FormatTokenDetail(nlohmann::json{
        {"models", nlohmann::json::array({nullptr})}}));
    CHECK_NOTHROW(FormatTokenDetail(nlohmann::json{
        {"models", nlohmann::json::array({nlohmann::json{{"input_tokens", "oops"},
                                                          {"output_tokens", true}}})}}));
}
