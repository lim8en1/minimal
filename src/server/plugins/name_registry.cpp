#include "minimal/plugins/name_registry.hpp"

namespace minimal::plugins {
namespace {

std::string StableIdentity(const std::string& plugin_name, const std::string& original_name) {
    return plugin_name + "." + original_name;
}

}  // namespace

void NameRegistry::Register(NameKind kind, std::string plugin_name, std::string original_name) {
    entries_[static_cast<int>(kind)].push_back({std::move(plugin_name), std::move(original_name)});
}

void NameRegistry::SetAlias(NameKind kind, std::string qualified_key, std::string alias) {
    aliases_[static_cast<int>(kind)][std::move(qualified_key)] = std::move(alias);
}

NameRegistry::Result NameRegistry::Resolve() const {
    Result result;

    for (const auto& [kind, kind_entries] : entries_) {
        const auto alias_it = aliases_.find(kind);
        const std::unordered_map<std::string, std::string>* kind_aliases =
            (alias_it != aliases_.end()) ? &alias_it->second : nullptr;

        std::unordered_map<std::string, int> original_name_counts;
        for (const auto& e : kind_entries) ++original_name_counts[e.original_name];

        std::unordered_map<std::string, int> surface_name_counts;
        std::vector<ResolvedEntry> kind_resolved;
        kind_resolved.reserve(kind_entries.size());

        for (const auto& e : kind_entries) {
            const std::string stable_identity = StableIdentity(e.plugin_name, e.original_name);

            std::string surface_name = e.original_name;
            if (kind_aliases) {
                if (auto it = kind_aliases->find(stable_identity); it != kind_aliases->end()) {
                    surface_name = it->second;
                }
            }

            const std::string permission_key =
                (original_name_counts[e.original_name] > 1) ? stable_identity : e.original_name;

            ++surface_name_counts[surface_name];
            kind_resolved.push_back({stable_identity, surface_name, permission_key});
        }

        for (const auto& [surface, count] : surface_name_counts) {
            if (count > 1) result.fatal_collisions.push_back(surface);
        }

        if (kind_aliases) {
            for (const auto& [qualified_key, alias] : *kind_aliases) {
                (void)alias;
                bool found = false;
                for (const auto& e : kind_entries) {
                    if (StableIdentity(e.plugin_name, e.original_name) == qualified_key) {
                        found = true;
                        break;
                    }
                }
                if (!found) result.dangling_aliases.push_back(qualified_key);
            }
        }

        for (auto& r : kind_resolved) result.entries.push_back(std::move(r));
    }

    // Kinds with no registrations at all never enter the loop above -- an alias
    // naming a plugin that is disabled or absent would otherwise be silently
    // kept, which is the exact typo the dangling check exists to catch.
    for (const auto& [kind, kind_aliases] : aliases_) {
        if (entries_.count(kind) != 0) continue;  // already covered above
        for (const auto& [qualified_key, alias] : kind_aliases) {
            (void)alias;
            result.dangling_aliases.push_back(qualified_key);
        }
    }

    return result;
}

}  // namespace minimal::plugins
