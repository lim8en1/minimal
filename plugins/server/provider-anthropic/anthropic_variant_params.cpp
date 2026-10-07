#include "anthropic_variant_params.hpp"

#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "minimal/common/logger.hpp"

#include "policy.hpp"
#include "api_limits.hpp"

namespace minimal::provider_anthropic {

namespace {

using nlohmann::json;

// Anthropic thinking budgets per effort name, for models that only take a
// budget (pre-adaptive: Haiku 4.5 and older). A scale, not data: values sit
// well under every such model's output cap, and the caller clamps against the
// base's max_tokens when known (the API rejects budget_tokens >= max_tokens);
// the request builder clamps again against the per-request cap.
std::optional<int> AnthropicBudgetFor(const std::string& effort_word) {
    for (const auto& [word, budget] : kThinkingBudgets) {
        if (effort_word == word) return budget;
    }
    return std::nullopt;  // "minimal": no thinking block at all
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
    if (capabilities == nullptr) {
        // Unreported: a current model -- but an older, budget-only one would
        // answer adaptive thinking with a 400, so say once how to declare it.
        static std::once_flag hinted;
        std::call_once(hinted, [] {
            common::DefaultLogger().info(
                "anthropic: a model with no capability report (discover: false) gets adaptive "
                "thinking for its @effort variants; for a budget-only model (Haiku 4.5, Sonnet "
                "4 and older) set \"thinking\": \"budget\" on its providers.json entry");
        });
        return {};
    }
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
    for (const auto& [rung, budget] : kThinkingBudgets) {  // the ladder, lowest first
        const std::string name(rung);
        if (effort.contains(name) && Supported(effort.at(name))) best = name;
        if (word == rung) return best;
    }
    return best;
}

}  // namespace

std::optional<std::pair<json, bool>> AnthropicVariantParams::DeriveParameters(
    const std::string& effort_word, std::optional<int> max_tokens, const json& metadata) const {
    if (effort_word == "none") {
        // Thinking omitted entirely: off where the model can turn it off, its
        // own default where it cannot -- never a 400 either way.
        return std::pair{json::object(), false};
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
        return std::pair{out, false};
    }
    if (!modes.budget) return std::pair{out, false};  // no thinking: effort alone, if any

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
        if (*max_tokens < kMinThinkingBudgetTokens + kReservedVisibleOutputTokens) {
            return std::pair{out, false};
        }
        const int ceiling = *max_tokens - kReservedVisibleOutputTokens;
        if (value > ceiling) {
            value = ceiling;
            clamped = true;
        }
    }
    out["thinking"] = {{"type", "enabled"}, {"budget_tokens", value}};
    return std::pair{out, clamped};
}

std::optional<std::vector<std::string>> AnthropicVariantParams::AdvertisedEfforts(
    const json& metadata) const {
    const json* capabilities = Capabilities(metadata);
    if (capabilities == nullptr) return std::nullopt;  // not ours: core's catalog shapes
    std::vector<std::string> out;
    // The effort rungs the model takes, in the ladder's order (object keys
    // carry none).
    if (capabilities->contains("effort") && Supported(capabilities->at("effort"))) {
        const json& effort = capabilities->at("effort");
        for (const auto& [rung, budget] : kThinkingBudgets) {
            const std::string name(rung);
            if (effort.contains(name) && Supported(effort.at(name))) out.push_back(name);
        }
        return out;
    }
    // No effort word, but budgeted thinking: the budget rungs, each a
    // thinking budget (DeriveParameters). Without them a budget-only model
    // had no @effort variants at all.
    if (ModesOf(capabilities).budget) {
        for (const auto& [rung, budget] : kThinkingBudgets) out.emplace_back(rung);
    }
    return out;
}

void AnthropicThinkingTranslator::Translate(json& entry, const std::string& where) const {
    if (!entry.is_object() || entry.value("backend", "") != "anthropic") return;
    if (!entry.contains("models") || !entry.at("models").is_object()) return;
    for (auto& [name, model] : entry.at("models").items()) {
        if (!model.is_object() || !model.contains("thinking")) continue;
        const json mode = model.at("thinking");
        model.erase("thinking");
        const std::string value = mode.is_string() ? mode.get<std::string>() : "";
        json capabilities;
        if (value == "adaptive") {
            capabilities = {{"thinking", {{"supported", true},
                                          {"types", {{"adaptive", {{"supported", true}}}}}}},
                            {"effort", {{"supported", true}}}};
            for (const auto& [rung, budget] : kThinkingBudgets) {
                capabilities["effort"][std::string(rung)] = {{"supported", true}};
            }
        } else if (value == "budget") {
            capabilities = {{"thinking", {{"supported", true},
                                          {"types", {{"enabled", {{"supported", true}}}}}}}};
        } else if (value == "none") {
            capabilities = {{"thinking", {{"supported", false}}}};
        } else {
            throw std::runtime_error(where + ": models." + name +
                                     ".thinking: must be \"adaptive\", \"budget\" or \"none\"");
        }
        json& metadata = model["metadata"];
        if (!metadata.is_object()) metadata = json::object();
        metadata["capabilities"] = std::move(capabilities);
    }
}

}  // namespace minimal::provider_anthropic
