#include "anthropic_variant_params.hpp"
#include "policy.hpp"

#include <stdexcept>

namespace minimal::provider_anthropic {

namespace {

// Anthropic thinking budgets per effort name -- moved verbatim from core's
// former AnthropicBudgetFor (providers_config.cpp). A scale, not data: no
// catalog publishes per-model budgets (models.dev carries per-model min/max
// in `reasoning_options`; a future catalog plugin can feed those through and
// this table becomes the fallback). Values sit well under every thinking
// model's output cap, and the caller clamps against the base's max_tokens
// when known (the API rejects budget_tokens >= max_tokens). Core passes the
// effort name verbatim, so every rung of the ladder gets its own budget.
std::optional<int> AnthropicBudgetFor(const std::string& effort_word) {
    for (const auto& [word, budget] : kThinkingBudgets) {
        if (effort_word == word) return budget;
    }
    return std::nullopt;  // "minimal": no thinking block at all
}

}  // namespace

std::pair<nlohmann::json, bool> AnthropicVariantParams::DeriveParameters(
    const std::string& effort_word, std::optional<int> max_tokens) const {
    if (effort_word == "none") {
        // Anthropic: thinking omitted entirely IS off.
        return {nlohmann::json::object(), false};
    }
    const auto budget = AnthropicBudgetFor(effort_word);
    if (!budget) {
        // "minimal" has no Anthropic reading; refuse here rather than send a
        // thinking map the API must 400.
        throw std::runtime_error(
            "providers.json: effort '" + effort_word +
            "' has no anthropic mapping (known: low, medium, high, xhigh, max, none)");
    }
    bool clamped = false;
    int value = *budget;
    if (max_tokens) {
        // The API rejects budget_tokens >= max_tokens; the ceiling below
        // leaves the model 1k of visible output, and a named warning beats a
        // first-turn 400. But the API ALSO rejects a budget under its own
        // minimum (1024) -- the ceiling alone had no floor, so a small
        // enough max_tokens (<=1024) drove `value` negative instead of just
        // small. A max_tokens too tight to hold both the minimum budget AND
        // the reserved output can't be salvaged by clamping: skip thinking
        // for this request rather than send a budget guaranteed invalid
        // either way, the same degrade "none"/"minimal" already use above.
        constexpr int kAnthropicMinBudgetTokens = 1024;  // NOLINT(readability-magic-numbers): the API's minimum
        if (*max_tokens < kAnthropicMinBudgetTokens + kReservedVisibleOutputTokens) {
            return {nlohmann::json::object(), false};
        }
        const int ceiling = *max_tokens - kReservedVisibleOutputTokens;
        if (value > ceiling) {
            value = ceiling;
            clamped = true;
        }
    }
    return {nlohmann::json{{"thinking", {{"type", "enabled"}, {"budget_tokens", value}}}}, clamped};
}

}  // namespace minimal::provider_anthropic
