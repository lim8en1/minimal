#include "minimal/llm/model_catalog.hpp"

#include <algorithm>
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
    return out;
}

}  // namespace minimal::llm
