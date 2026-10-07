#include "minimal/llm/model_catalog.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace minimal::llm {

bool ModelCatalogRegistry::Register(std::string kind, std::shared_ptr<ModelCatalog> catalog) {
    if (!catalog) return false;
    return AttributedRegistry::Register(std::move(kind), std::move(catalog));
}

ModelCatalog* ModelCatalogRegistry::Find(std::string_view kind) const {
    const auto* entry = FindEntry(kind);
    return entry == nullptr ? nullptr : entry->get();
}

std::vector<std::string> AdvertisedEfforts(const nlohmann::json& metadata) {
    std::vector<std::string> out;
    if (!metadata.is_object()) return out;
    // OpenRouter's live /models shape: a top-level "reasoning" object whose
    // "supported_efforts" is the ladder ({"mandatory": true,
    // "supported_efforts": ["max","high","low"], ...}).
    if (metadata.contains("reasoning") && metadata.at("reasoning").is_object()) {
        const auto& reasoning = metadata.at("reasoning");
        if (reasoning.contains("supported_efforts") && reasoning.at("supported_efforts").is_array()) {
            for (const auto& effort : reasoning.at("supported_efforts")) {
                if (effort.is_string() && !effort.get<std::string>().empty()) {
                    out.push_back(effort.get<std::string>());
                }
            }
            if (!out.empty()) return out;
        }
        // A reasoning object without a ladder is the toggle-only shape
        // ("mandatory": true with no efforts): no efforts either way.
        return out;
    }
    // Anthropic's /v1/models shape: "capabilities.effort" is a tree of
    // {"supported": bool} leaves, one per effort word, plus its own top-level
    // "supported". Object keys carry no order, so the ladder is read in the
    // vocabulary's own order.
    if (metadata.contains("capabilities") && metadata.at("capabilities").is_object()) {
        const auto& capabilities = metadata.at("capabilities");
        const auto supported = [](const nlohmann::json& node) {
            return node.is_object() && node.contains("supported") &&
                   node.at("supported").is_boolean() && node.at("supported").get<bool>();
        };
        if (capabilities.contains("effort") && supported(capabilities.at("effort"))) {
            const auto& effort = capabilities.at("effort");
            for (const char* word : {"low", "medium", "high", "xhigh", "max"}) {
                if (effort.contains(word) && supported(effort.at(word))) out.push_back(word);
            }
        }
        return out;
    }
    // models.dev's normalized shape: "reasoning_options" is an array of
    // {"type": "..."} records -- "effort" carries "values", "budget_tokens"
    // carries min/max (a budget, not a ladder), "toggle" is on/off.
    if (metadata.contains("reasoning_options") && metadata.at("reasoning_options").is_array()) {
        for (const auto& option : metadata.at("reasoning_options")) {
            if (!option.is_object()) continue;
            if (option.value("type", "") == "effort" && option.contains("values") &&
                option.at("values").is_array()) {
                for (const auto& effort : option.at("values")) {
                    if (effort.is_string() && !effort.get<std::string>().empty()) {
                        out.push_back(effort.get<std::string>());
                    }
                }
            }
        }
        return out;
    }
    // A LiteLLM proxy's /model/info row, attached as "model_info" by an
    // opted-in discovery: `supports_reasoning` means the proxy maps
    // reasoning_effort's standard low/medium/high for the model, and
    // per-level flags add the model-specific rungs (none/minimal/xhigh/max)
    // or, set false, withdraw a standard one. Flags present-but-null -- the
    // proxy's spelling for "not a reasoning model" -- say nothing.
    if (metadata.contains("model_info") && metadata.at("model_info").is_object()) {
        const auto& info = metadata.at("model_info");
        const auto flag = [&info](const std::string& name) -> std::optional<bool> {
            const auto it = info.find(name);
            if (it == info.end() || !it->is_boolean()) return std::nullopt;
            return it->get<bool>();
        };
        if (flag("supports_reasoning") != true) return out;
        static const std::pair<const char*, bool> kLadder[] = {
            {"none", false}, {"minimal", false}, {"low", true},  {"medium", true},
            {"high", true},  {"xhigh", false},   {"max", false},
        };
        for (const auto& [word, standard] : kLadder) {
            const auto level = flag(std::string("supports_") + word + "_reasoning_effort");
            if (level.value_or(standard)) out.push_back(word);
        }
    }
    return out;
}

}  // namespace minimal::llm
