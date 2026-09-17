#include "minimal/llm/model_catalog.hpp"

#include <algorithm>

namespace minimal::llm {

bool ModelCatalogRegistry::Register(std::string kind, std::shared_ptr<ModelCatalog> catalog) {
    if (!catalog) return false;
    const std::string key = kind;  // read before the move below
    const bool inserted = catalogs_.emplace(std::move(kind), std::move(catalog)).second;
    if (inserted) {
        registrant_[key] = attribution_;
        return true;
    }
    // Recorded, not just refused. The loser is dropped either way -- the
    // collision is fatal upstream, so which object holds the slot is moot --
    // but a silent drop made the winner depend on load order with nothing to
    // point an operator at (see ProviderFactoryRegistry::Register, the same
    // pattern for the sibling registry).
    const auto first = registrant_.find(key);
    collisions_.push_back(Collision{key, first != registrant_.end() ? first->second : std::string{},
                                    attribution_});
    return false;
}

ModelCatalog* ModelCatalogRegistry::Find(std::string_view kind) const {
    const auto it = catalogs_.find(kind);
    return it == catalogs_.end() ? nullptr : it->second.get();
}

bool ModelCatalogRegistry::Has(std::string_view kind) const {
    return catalogs_.find(kind) != catalogs_.end();
}

std::vector<std::string> ModelCatalogRegistry::kinds() const {
    std::vector<std::string> out;
    out.reserve(catalogs_.size());
    for (const auto& [kind, _] : catalogs_) out.push_back(kind);
    return out;  // map iteration is already sorted
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
    }
    return out;
}

}  // namespace minimal::llm
