#pragma once

// Pure formatters for the cost status segment -- header-only so tests can
// include them directly (the logo_generator pure-extraction pattern: logic
// out of the plugin TU, no plugin machinery needed to test it).
//
// Rules the tests pin down:
//   - "$0.00" must never stand in for "unknown": an absent or non-finite
//     cost renders as the literal "cost?" marker, chosen by FormatTotals.
//   - Token counts tier at k/M/G; extreme values never truncate mid-number.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace minimal::client::cost_view {

// "1234567" -> "1.2M", "12345" -> "12.3k", "123" -> "123", >=1e9 -> G tier.
inline std::string FormatTokens(std::int64_t tokens) {
    if (tokens >= 1'000'000'000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fG", static_cast<double>(tokens) / 1e9);
        return buf;
    }
    if (tokens >= 1'000'000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fM", static_cast<double>(tokens) / 1e6);
        return buf;
    }
    if (tokens >= 1'000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fk", static_cast<double>(tokens) / 1e3);
        return buf;
    }
    return std::to_string(tokens);
}

// Sub-cent totals are common early in a session; four decimals avoids "$0.00"
// reading as free when it is actually just small. The caller is responsible
// for never handing a negative or unknown total here (see FormatTotals).
inline std::string FormatCost(double usd) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), usd < 0.01 ? "$%.4f" : "$%.2f", usd);
    return buf;
}

// nullopt: no usable totals yet for this session (a fresh session with zero
// turns, a parse failure, a refused/unreachable call) -- Render() shows
// nothing for those, same as any other unfilled segment.
inline std::optional<std::string> FormatTotals(const nlohmann::json& totals) {
    if (!totals.is_object()) return std::nullopt;
    const auto turns = totals.value("turns", 0);
    if (turns <= 0) return std::nullopt;   // nothing spent yet -- not "$0.00"

    std::int64_t input_tokens = 0, output_tokens = 0;
    if (const auto models = totals.find("models"); models != totals.end() && models->is_array()) {
        for (const auto& model : *models) {
            input_tokens += model.value("input_tokens", 0LL);
            output_tokens += model.value("output_tokens", 0LL);
        }
    }

    std::string text;
    // A missing price is not a price of zero (cost_model.hpp's own rule) --
    // an unknown total renders as "cost?", never as a wrong dollar figure.
    // The DEFAULT is unknown: the field's own default-true made any blob
    // written before the field existed (or with the fields missing) render
    // "$0.0000" -- free. A NEGATIVE total is the same kind of anomaly.
    const bool known = totals.value("total_cost_known", false);
    const double total = totals.value("total_cost", 0.0);
    if (known && total >= 0.0) {
        text = FormatCost(total);
    } else {
        text = "cost?";
    }
    text += " · " + FormatTokens(input_tokens) + "/" + FormatTokens(output_tokens) + " tok";
    if (totals.value("unpriced_turns", 0) > 0) text += " (+unpriced)";
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
    std::int64_t input_tokens = 0, output_tokens = 0;
    std::int64_t cache_read = 0, cache_write = 0;
    if (const auto models = totals.find("models"); models != totals.end() && models->is_array()) {
        for (const auto& model : *models) {
            input_tokens += model.value("input_tokens", 0LL);
            output_tokens += model.value("output_tokens", 0LL);
            cache_read += model.value("cache_read_tokens", 0LL);
            cache_write += model.value("cache_write_tokens", 0LL);
        }
    }
    std::string text;
    const auto append = [&text](const char* label, std::int64_t tokens) {
        if (tokens <= 0) return;   // suppressed -- and negatives are corruption, not a refund
        if (!text.empty()) text += " · ";
        text += label;
        text += ' ';
        text += FormatTokens(tokens);
    };
    append("i", input_tokens);
    append("o", output_tokens);
    append("cr", cache_read);
    append("cw", cache_write);
    if (text.empty()) return std::nullopt;
    return text;
}

}  // namespace minimal::client::cost_view
