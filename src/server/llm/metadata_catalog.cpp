#include "minimal/llm/metadata_catalog.hpp"

namespace minimal::llm {

bool MetadataCatalogRegistry::Register(std::string kind, std::shared_ptr<MetadataCatalog> catalog) {
    if (!catalog) return false;
    return AttributedRegistry::Register(std::move(kind), std::move(catalog));
}

bool MetadataCatalogRegistry::RegisterFallback(std::shared_ptr<MetadataCatalog> catalog) {
    if (!catalog) return false;
    for (const auto& existing : fallbacks_) {
        if (existing.get() == catalog.get()) return false;
    }
    fallbacks_.push_back(std::move(catalog));
    fallback_plugins_.push_back(attribution());
    return true;
}

MetadataCatalog* MetadataCatalogRegistry::Find(std::string_view kind) const {
    const auto* entry = FindEntry(kind);
    return entry == nullptr ? nullptr : entry->get();
}

bool MetadataCatalogRegistry::HasFallbackFrom(const std::string& plugin_name) const {
    for (const auto& plugin : fallback_plugins_) {
        if (plugin == plugin_name) return true;
    }
    return false;
}

void MetadataCatalogRegistry::RemoveByAttribution(const std::string& plugin_name) {
    AttributedRegistry::RemoveByAttribution(plugin_name);
    for (std::size_t i = fallbacks_.size(); i-- > 0;) {
        if (fallback_plugins_[i] == plugin_name) {
            fallbacks_.erase(fallbacks_.begin() + static_cast<std::ptrdiff_t>(i));
            fallback_plugins_.erase(fallback_plugins_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

MetadataCatalog* MetadataCatalogRegistry::FindForQuery(const MetadataQuery& query,
                                                       MetadataMatch match) const {
    if (MetadataCatalog* own = Find(query.backend); own != nullptr) return own;
    // First registered fallback that ANSWERS wins; a nullopt answer falls
    // through to the next. Lookup is allowed to throw -- treated here as "no
    // answer", with the caller's logging free to report it: a broken
    // enrichment catalog must not break resolution, which is the one promise
    // this seam makes.
    for (const auto& fallback : fallbacks_) {
        try {
            if (fallback->Lookup(query, match).has_value()) return fallback.get();
        } catch (...) {
        }
    }
    return nullptr;
}

}  // namespace minimal::llm
