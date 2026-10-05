#include "minimal/plugins/name_table.hpp"

#include <algorithm>

#include "minimal/common/logger.hpp"

namespace minimal::plugins {

std::string StableIdentity(std::string_view plugin_name, std::string_view original_name) {
    std::string identity;
    identity.reserve(plugin_name.size() + 1 + original_name.size());
    identity.append(plugin_name).append(".").append(original_name);
    return identity;
}

void NameTable::Register(std::string plugin_name, std::string original_name) {
    if (plugin_name.find('.') != std::string::npos) {
        common::DefaultLogger().warn(
            "plugin name '{}' contains a dot, which is the qualified-key separator -- "
            "alias and collision identity for its names is ambiguous", plugin_name);
    }
    std::string identity = StableIdentity(plugin_name, original_name);
    identities_.insert(identity);
    registrations_.push_back({std::move(plugin_name), std::move(original_name), std::move(identity)});
}

bool NameTable::Claims(std::string_view qualified_key) const {
    return identities_.count(std::string(qualified_key)) != 0;
}

void NameTable::SetAlias(std::string qualified_key, std::string alias) {
    if (policy_ == Policy::kSlot) {
        common::DefaultLogger().warn("alias '{}' targets a non-aliasable key; ignored",
                                     qualified_key);
        return;
    }
    aliases_[std::move(qualified_key)] = std::move(alias);
}

NameTable::Result NameTable::Resolve() const {
    Result result;
    result.entries.reserve(registrations_.size());

    // surface -> claimants, in first-registration order. std::map keeps the
    // collision report sorted by surface.
    std::map<std::string, std::vector<std::string>> claimants;
    std::unordered_map<std::string, int> identity_counts;
    for (const auto& r : registrations_) {
        Entry entry{r.plugin_name, r.original_name, r.stable_identity, r.original_name, false};
        if (const auto it = aliases_.find(r.stable_identity); it != aliases_.end()) {
            entry.surface_name = it->second;
            entry.aliased = true;
        }
        auto& owners = claimants[entry.surface_name];
        owners.push_back(r.plugin_name);
        ++identity_counts[r.stable_identity];
        result.entries.push_back(std::move(entry));
    }

    for (auto& [surface, owners] : claimants) {
        if (owners.size() < 2) continue;
        std::vector<std::string> unique;
        for (auto& owner : owners) {
            if (std::find(unique.begin(), unique.end(), owner) == unique.end()) {
                unique.push_back(std::move(owner));
            }
        }
        result.collisions.push_back({surface, std::move(unique)});
    }
    for (const auto& [identity, count] : identity_counts) {
        if (count > 1) result.ambiguous_identities.push_back(identity);
    }
    for (const auto& [key, alias] : aliases_) {
        if (identities_.count(key) == 0) result.dangling_aliases.push_back(key);
    }
    std::sort(result.ambiguous_identities.begin(), result.ambiguous_identities.end());
    std::sort(result.dangling_aliases.begin(), result.dangling_aliases.end());
    return result;
}

AliasRouting RouteAliases(const std::map<std::string, std::string>& aliases,
                          std::span<NameTable* const> tables) {
    AliasRouting routing;
    for (const auto& [key, alias] : aliases) {
        bool named = false;
        bool slotted = false;
        for (NameTable* table : tables) {
            if (!table->Claims(key)) continue;
            if (table->policy() == NameTable::Policy::kName) {
                table->SetAlias(key, alias);
                named = true;
            } else {
                slotted = true;
            }
        }
        if (named) continue;
        (slotted ? routing.targets_slot : routing.dangling).push_back(key);
    }
    return routing;  // std::map iteration: already sorted
}

}  // namespace minimal::plugins
