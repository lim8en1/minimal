#include "minimal/llm/model_catalog.hpp"

#include <algorithm>

namespace minimal::llm {

bool ModelCatalogRegistry::Register(std::string kind, std::shared_ptr<ModelCatalog> catalog) {
    if (!catalog) return false;
    return catalogs_.emplace(std::move(kind), std::move(catalog)).second;
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

}  // namespace minimal::llm
