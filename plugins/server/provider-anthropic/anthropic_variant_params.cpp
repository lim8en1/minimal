#include "anthropic_variant_params.hpp"

#include <array>
#include <stdexcept>

namespace minimal::provider_anthropic {

namespace {

using nlohmann::json;

// The effort ladder, lowest first: the order step-down walks.
constexpr std::array<const char*, 5> kLadder = {"low", "medium", "high", "xhigh", "max"};

// Anthropic thinking budgets per effort name, for models that only take a
// budget (pre-adaptive: Haiku 4.5 and older). A scale, not data: values sit
// well under every such model's output cap, and the caller clamps against the
// base's max_tokens when known (the API rejects budget_tokens >= max_tokens);
// the request builder clamps again against the per-request cap.
std::optional<int> AnthropicBudgetFor(const std::string& effort_word) {
    if (effort_word == "low") return 4096;
    if (effort_word == "medium") return 8192;
    if (effort_word == "high") return 16384;
    if (effort_word == "xhigh") return 32768;
    if (effort_word == "max") return 65536;
    return std::nullopt;
}

bool Supported(const json& node) {
    return node.is_object() && node.contains("supported") && node.at("supported").is_boolean() &&
           node.at("supported").get<bool>();
}

// The /v1/models capability report, or nullptr when the model has none.
const json* Capabilities(const json& metadata) {
    if (!metadata.is_object() || !metadata.contains("capabilities")) return nullptr;
    const json& capabilities = metadata.at("capabilities");
    return capabilities.is_object() ? &capabilities : nullptr;
}

struct ThinkingModes {
    bool adaptive = true;
    bool budget = false;
};

ThinkingModes ModesOf(const json* capabilities) {
    if (capabilities == nullptr) return {};  // unreported: a current model
    ThinkingModes modes{false, false};
    if (!capabilities->contains("thinking")) return modes;
    const json& thinking = capabilities->at("thinking");
    if (!Supported(thinking) || !thinking.contains("types") || !thinking.at("types").is_object()) {
        return modes;
    }
    const json& types = thinking.at("types");
    modes.adaptive = types.contains("adaptive") && Supported(types.at("adaptive"));
    modes.budget = types.contains("enabled") && Supported(types.at("enabled"));
    return modes;
}

// The effort word to send: `word` if the model takes it, else the nearest
// lower rung it does take; nullopt when the model takes no effort at or below
// `word` (or none at all).
std::optional<std::string> EffortFor(const json* capabilities, const std::string& word) {
    if (capabilities == nullptr) return word;  // unreported: every rung
    if (!capabilities->contains("effort") || !Supported(capabilities->at("effort"))) {
        return std::nullopt;
    }
    const json& effort = capabilities->at("effort");
    std::optional<std::string> best;
    for (const char* rung : kLadder) {
        if (effort.contains(rung) && Supported(effort.at(rung))) best = rung;
        if (word == rung) return best;
    }
    return best;
}

}  // namespace

std::pair<json, bool> AnthropicVariantParams::DeriveParameters(const std::string& effort_word,
                                                               std::optional<int> max_tokens,
                                                               const json& metadata) const {
    if (effort_word == "none") {
        // Thinking omitted entirely: off where the model can turn it off, its
        // own default where it cannot -- never a 400 either way.
        return {json::object(), false};
    }
    const auto budget = AnthropicBudgetFor(effort_word);
    if (!budget) {
        // "minimal" has no Anthropic reading; refuse here rather than send a
        // thinking map the API must 400.
        throw std::runtime_error(
            "providers.json: effort '" + effort_word +
            "' has no anthropic mapping (known: low, medium, high, xhigh, max, none)");
    }

    const json* capabilities = Capabilities(metadata);
    const ThinkingModes modes = ModesOf(capabilities);
    json out = json::object();
    if (const auto effort = EffortFor(capabilities, effort_word)) {
        out["output_config"] = {{"effort", *effort}};
    }

    if (modes.adaptive) {
        // "summarized": newer models default to "omitted" (empty thinking
        // text), which would turn the transcript's reasoning rows blank.
        out["thinking"] = {{"type", "adaptive"}, {"display", "summarized"}};
        return {out, false};
    }
    if (!modes.budget) return {out, false};  // no thinking: effort alone, if any

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
        constexpr int kAnthropicMinBudgetTokens = 1024;
        constexpr int kReservedVisibleOutput = 1024;
        if (*max_tokens < kAnthropicMinBudgetTokens + kReservedVisibleOutput) {
            return {out, false};
        }
        const int ceiling = *max_tokens - kReservedVisibleOutput;
        if (value > ceiling) {
            value = ceiling;
            clamped = true;
        }
    }
    out["thinking"] = {{"type", "enabled"}, {"budget_tokens", value}};
    return {out, clamped};
}

}  // namespace minimal::provider_anthropic
