#include "minimal/llm/variant_params.hpp"

namespace minimal::llm {

bool VariantParamsRegistry::Register(std::string kind, std::shared_ptr<VariantParamsProvider> provider) {
    if (!provider) return false;
    const std::string key = kind;  // read before the move below
    const bool inserted = providers_.emplace(std::move(kind), std::move(provider)).second;
    if (inserted) {
        registrant_[key] = attribution_;
        return true;
    }
    // Recorded, not just refused -- see ModelCatalogRegistry::Register's
    // identical comment: a silent drop makes the winner depend on load order
    // with nothing to point an operator at.
    const auto first = registrant_.find(key);
    collisions_.push_back(Collision{key, first != registrant_.end() ? first->second : std::string{},
                                    attribution_});
    return false;
}

VariantParamsProvider* VariantParamsRegistry::Find(std::string_view kind) const {
    const auto it = providers_.find(kind);
    return it == providers_.end() ? nullptr : it->second.get();
}

bool VariantParamsRegistry::Has(std::string_view kind) const {
    return providers_.find(kind) != providers_.end();
}

std::vector<std::string> VariantParamsRegistry::kinds() const {
    std::vector<std::string> out;
    out.reserve(providers_.size());
    for (const auto& [kind, _] : providers_) out.push_back(kind);
    return out;  // map iteration is already sorted
}

}  // namespace minimal::llm
