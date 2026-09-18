#include "minimal/llm/metadata_catalog.hpp"

namespace minimal::llm {

bool MetadataCatalogRegistry::Register(std::string kind, std::shared_ptr<MetadataCatalog> catalog) {
    if (!catalog) return false;
    const std::string key = kind;  // read before the move below
    const bool inserted = catalogs_.emplace(std::move(kind), std::move(catalog)).second;
    if (inserted) {
        registrant_[key] = attribution_;
        return true;
    }
    // Recorded, not just refused -- the same rule as ModelCatalogRegistry:
    // a silent drop makes the winner depend on load order with nothing to
    // point an operator at.
    const auto first = registrant_.find(key);
    collisions_.push_back(Collision{key, first != registrant_.end() ? first->second : std::string{},
                                    attribution_});
    return false;
}

bool MetadataCatalogRegistry::RegisterFallback(std::shared_ptr<MetadataCatalog> catalog) {
    if (!catalog) return false;
    // One fallback per instance: a plugin registering the same catalog twice
    // would make the first-answering-fallback rule moot and double-log every
    // miss. Comparison by owner-less pointer identity is exactly the check.
    for (const auto& existing : fallbacks_) {
        if (existing.get() == catalog.get()) return false;
    }
    fallbacks_.push_back(std::move(catalog));
    return true;
}

MetadataCatalog* MetadataCatalogRegistry::Find(std::string_view kind) const {
    const auto it = catalogs_.find(kind);
    return it == catalogs_.end() ? nullptr : it->second.get();
}

MetadataCatalog* MetadataCatalogRegistry::FindForQuery(const MetadataQuery& query) const {
    if (MetadataCatalog* own = Find(query.backend); own != nullptr) return own;
    // First registered fallback that ANSWERS wins; a nullopt answer falls
    // through to the next. Lookup is allowed to throw -- treated here as "no
    // answer", with the caller's logging free to report it: a broken
    // enrichment catalog must not break resolution, which is the one promise
    // this seam makes.
    for (const auto& fallback : fallbacks_) {
        try {
            if (fallback->Lookup(query, MetadataMatch::Full).has_value()) return fallback.get();
        } catch (...) {
        }
    }
    return nullptr;
}

bool MetadataCatalogRegistry::Has(std::string_view kind) const {
    return catalogs_.find(kind) != catalogs_.end();
}

std::vector<std::string> MetadataCatalogRegistry::kinds() const {
    std::vector<std::string> out;
    out.reserve(catalogs_.size());
    for (const auto& [kind, _] : catalogs_) out.push_back(kind);
    return out;  // map iteration is already sorted
}

}  // namespace minimal::llm
