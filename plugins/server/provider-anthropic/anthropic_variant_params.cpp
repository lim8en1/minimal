#include "anthropic_variant_params.hpp"

#include <stdexcept>

namespace minimal::provider_anthropic {

namespace {

// Anthropic thinking budgets per effort name -- moved verbatim from core's
// former AnthropicBudgetFor (providers_config.cpp). A scale, not data: no
// catalog publishes per-model budgets (models.dev carries per-model min/max
// in `reasoning_options`; a future catalog plugin can feed those through and
// this table becomes the fallback). Values sit well under every thinking
// model's output cap, and the caller clamps against the base's max_tokens
// when known (the API rejects budget_tokens >= max_tokens). Only
// low/medium/high are checked here -- the dispatcher in core's
// VariantParametersFor already folds "xhigh"/"max" to "high" before this
// provider is ever consulted.
std::optional<int> AnthropicBudgetFor(const std::string& effort_word) {
    if (effort_word == "low") return 4096;
    if (effort_word == "medium") return 8192;
    if (effort_word == "high") return 16384;
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
    if (max_tokens && value > *max_tokens - 1024) {
        // The one cross-field constraint the parser can see coming: the API
        // rejects budget_tokens >= max_tokens. Leave the model 1k of visible
        // output; a named warning beats a first-turn 400.
        value = *max_tokens - 1024;
        clamped = true;
    }
    return {nlohmann::json{{"thinking", {{"type", "enabled"}, {"budget_tokens", value}}}}, clamped};
}

}  // namespace minimal::provider_anthropic
