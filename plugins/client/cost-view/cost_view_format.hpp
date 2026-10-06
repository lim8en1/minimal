#pragma once

// Pure formatters for the cost status segment -- header-only so tests can
// include them directly (the logo_generator pure-extraction pattern: logic
// out of the plugin TU, no plugin machinery needed to test it).
//
// Rules the tests pin down:
//   - "$0.00" must never stand in for "unknown": an absent or non-finite
//     cost renders as the literal "cost?" marker, chosen by FormatTotals.
//   - A PARTIALLY known total (some models priced, some not -- a mid-session
//     model switch is the common case) renders as a ">= $X" floor, never as
//     the bare "cost?" marker: the real accumulated dollars are real, even
//     though the true total could be higher. Same three-way rule
//     cost_command.cpp's CostCell() uses for the /cost table.
//   - Token counts tier at k/M/G; extreme values never truncate mid-number.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>
#include "minimal/common/count_format.hpp"

namespace minimal::client::cost_view {

// Appended to a cached segment string the push stream is no longer keeping
// current (totals_feed.hpp) -- the last-known figure, marked, rather than
// either blanking it or passing it off as live.
inline constexpr const char* kStaleSuffix = " (stale)";

// Like nlohmann's own `j.value(key, fallback)`, but never throws: `j.value()`
// raises type_error.302 if the field is present with the WRONG type (an int
// where a bool was expected) and type_error.306 if `j` itself is not an
// object at all (a non-object entry inside a "models" array). The totals
// payload these formatters read comes over PluginCall from another plugin's
// RPC reply -- a shape this process does not control -- and the caller here
// is a background poller's std::thread entry function, where an uncaught
// exception is std::terminate(), not a caught error: a malformed reply must
// degrade the segment, never kill the client. `find()` (unlike `value()`) is
// itself safe on a non-object `j`, returning end() rather than throwing.
template <typename T>
T SafeValue(const nlohmann::json& j, const char* key, T fallback) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    bool compatible;
    if constexpr (std::is_same_v<T, bool>) {
        compatible = it->is_boolean();
    } else if constexpr (std::is_integral_v<T>) {
        compatible = it->is_number_integer();
    } else if constexpr (std::is_floating_point_v<T>) {
        compatible = it->is_number();
    } else {
        compatible = it->is_string();
    }
    if (!compatible) return fallback;
    return it->get<T>();
}

// Sub-cent totals are common early in a session; four decimals avoids "$0.00"
// reading as free when it is actually just small. The caller is responsible
// for never handing a negative or unknown total here (see FormatTotals).
inline std::string FormatCost(double usd) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), usd < 0.01 ? "$%.4f" : "$%.2f", usd);
    return buf;
}

// The four token sums cost-tracker's totals RPC carries per model, summed
// across every model in the "models" array. Shared by FormatTotals (which
// only needs input/output) and FormatTokenDetail (which needs all four) --
// they used to duplicate this loop verbatim, a future change to token
// aggregation (a new field, a different truncation rule) having to happen in
// two places.
struct TokenTotals {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read = 0;
    std::int64_t cache_write = 0;
};

inline TokenTotals AggregateModelTokens(const nlohmann::json& totals) {
    // A negative per-model count is corruption, not a refund: it counts as
    // zero rather than eating into the other models' real usage.
    const auto count = [](const nlohmann::json& model, const char* key) {
        const auto value = SafeValue<std::int64_t>(model, key, 0LL);
        return value > 0 ? value : std::int64_t{0};
    };
    TokenTotals sum;
    if (const auto models = totals.find("models"); models != totals.end() && models->is_array()) {
        for (const auto& model : *models) {
            sum.input_tokens += count(model, "input_tokens");
            sum.output_tokens += count(model, "output_tokens");
            sum.cache_read += count(model, "cache_read_tokens");
            sum.cache_write += count(model, "cache_write_tokens");
        }
    }
    return sum;
}

// nullopt: no usable totals yet for this session (a fresh session with zero
// turns, a parse failure, a refused/unreachable call) -- Render() shows
// nothing for those, same as any other unfilled segment.
inline std::optional<std::string> FormatTotals(const nlohmann::json& totals) {
    if (!totals.is_object()) return std::nullopt;
    const auto turns = SafeValue<int>(totals, "turns", 0);
    if (turns <= 0) return std::nullopt;   // nothing spent yet -- not "$0.00"

    const TokenTotals tokens = AggregateModelTokens(totals);

    std::string text;
    // A missing price is not a price of zero (cost_model.hpp's own rule) --
    // an unknown total renders as "cost?", never as a wrong dollar figure.
    // The DEFAULT is unknown: the field's own default-true made any blob
    // written before the field existed (or with the fields missing) render
    // "$0.0000" -- free. A NEGATIVE total is the same kind of anomaly.
    const bool known = SafeValue<bool>(totals, "total_cost_known", false);
    const double total = SafeValue<double>(totals, "total_cost", 0.0);
    if (known && total >= 0.0) {
        text = FormatCost(total);
    } else if (total > 0.0) {
        // Partially known: at least one model's turns priced (the sum is
        // real), at least one did not (a model switch mid-session is the
        // common trigger). A floor beats discarding the real dollars spent.
        text = ">= " + FormatCost(total);
    } else {
        text = "cost?";
    }
    text += " · " + common::CompactCount(tokens.input_tokens) + "/" + common::CompactCount(tokens.output_tokens) +
           " tok";
    if (SafeValue<int>(totals, "unpriced_turns", 0) > 0) text += " (+unpriced)";
    return text;
}

// Cumulative token breakdown for the {token_detail} status segment -- the
// same "totals" payload FormatTotals reads, but the four token sums the
// cost-tracker already aggregates per model (input/output plus the cache
// read/write fields its totals RPC has always carried) rendered raw, without
// the dollar figure. Zero fields render NOTHING, not "cw 0": an OpenAI-shaped
// backend has no cache-write concept, so cw is always 0 there and the field
// is pure noise (cache_view rules: unknown/unfilled renders nothing). An
// all-zero aggregate -- a fresh session, or a backend that reported nothing
// -- renders nothing at all. nullopt for a non-object payload, the same
// malformed-input contract as FormatTotals.
inline std::optional<std::string> FormatTokenDetail(const nlohmann::json& totals) {
    if (!totals.is_object()) return std::nullopt;
    const TokenTotals tokens = AggregateModelTokens(totals);
    std::string text;
    const auto append = [&text](const char* label, std::int64_t count) {
        if (count <= 0) return;   // suppressed -- and negatives are corruption, not a refund
        if (!text.empty()) text += " · ";
        text += label;
        text += ' ';
        text += common::CompactCount(count);
    };
    append("i", tokens.input_tokens);
    append("o", tokens.output_tokens);
    append("cr", tokens.cache_read);
    append("cw", tokens.cache_write);
    if (text.empty()) return std::nullopt;
    return text;
}

}  // namespace minimal::client::cost_view
