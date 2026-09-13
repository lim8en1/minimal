#include "minimal/plugins/name_registry.hpp"

#include "minimal/common/logger.hpp"

namespace minimal::plugins {
namespace {

std::string StableIdentity(const std::string& plugin_name, const std::string& original_name) {
    return plugin_name + "." + original_name;
}

}  // namespace

void NameRegistry::Register(NameKind kind, std::string plugin_name, std::string original_name) {
    // The dot is the qualified-key separator ("plugin.item"): a plugin NAMED
    // with a literal dot makes ("a.b", "c") and ("a", "b.c") compose the
    // same stable identity, and an alias written for one silently matches
    // the other. Nothing escapes dots today, so this stays AMBIGUOUS-by-
    // convention -- visible here instead of invisible everywhere.
    if (plugin_name.find('.') != std::string::npos) {
        common::DefaultLogger().warn(
            "plugin name '{}' contains a dot, which is the qualified-key separator -- "
            "alias and collision identity for its names is ambiguous", plugin_name);
    }
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

        // First pass: resolve each entry's alias (if any). permission_key
        // ambiguity is judged on UNALIASED original_name counts, not raw ones
        // -- an entry that keeps its original_name as its surface is still
        // addressed by that bare name, but one that has been aliased away
        // already has a distinct identity (its alias, or failing that its
        // stable identity) and no longer needs to reserve the bare name for
        // itself. Without this split, aliasing away one half of a collision
        // (the documented remedy -- see SetAlias) permanently qualified the
        // OTHER half's permission_key too, because both entries still share
        // the same literal original_name: a pre-existing `permissions:
        // {"read": "deny"}` rule for a built-in "read" tool stopped matching
        // the moment an unrelated plugin registered a colliding "read" and the
        // operator aliased the plugin's copy away, even though only one tool
        // still answers to "read" at all afterward.
        struct Resolved {
            std::string stable_identity;
            std::string surface_name;
            bool aliased;
        };
        std::vector<Resolved> resolved;
        resolved.reserve(kind_entries.size());
        std::unordered_map<std::string, int> unaliased_name_counts;
        std::unordered_map<std::string, int> total_name_counts;
        std::unordered_map<std::string, int> identity_counts;
        for (const auto& e : kind_entries) {
            std::string stable_identity = StableIdentity(e.plugin_name, e.original_name);
            std::string surface_name = e.original_name;
            bool aliased = false;
            if (kind_aliases) {
                if (auto it = kind_aliases->find(stable_identity); it != kind_aliases->end()) {
                    surface_name = it->second;
                    aliased = true;
                }
            }
            if (!aliased) ++unaliased_name_counts[e.original_name];
            ++total_name_counts[e.original_name];
            resolved.push_back({std::move(stable_identity), std::move(surface_name), aliased});
        }

        std::unordered_map<std::string, int> surface_name_counts;
        std::vector<ResolvedEntry> kind_resolved;
        kind_resolved.reserve(kind_entries.size());

        for (std::size_t i = 0; i < kind_entries.size(); ++i) {
            const auto& e = kind_entries[i];
            const auto& r = resolved[i];

            // The composition-ambiguity check, per kind: two (plugin, name)
            // pairs whose StableIdentity strings coincide -- ("a.b","c") and
            // ("a","b.c") both compose "a.b.c" -- cannot be told apart by
            // ANY qualified key, so an alias or rule targeting one silently
            // governs both. That is the case the Register-time dot warning
            // only gestures at; here, where the whole registered set is
            // visible, it becomes startup-fatal. A LONE dotted plugin name
            // composes nothing and stays the warning it always was.
            identity_counts[r.stable_identity]++;

            // Bare in two disjoint cases:
            //  - no other entry, aliased or not, ever claimed this
            //    original_name -- there was never any ambiguity to resolve, so
            //    aliasing a single tool for typing convenience must not detach
            //    its existing per-tool rule (docs/CONFIGURATION.md: "aliased
            //    tools keep their original name" in permission config); or
            //  - this entry kept its original_name as its surface AND no other
            //    unaliased entry still claims that name -- the collision was
            //    resolved by moving every other claimant away.
            // An entry that itself moved away, while another original_name
            // claimant remains (aliased or not), keeps its stable identity:
            // it and that claimant are still two distinct tools that once
            // shared a name, so a rule keyed on the bare name must not
            // silently govern both.
            const bool never_contested = total_name_counts[e.original_name] <= 1;
            const bool collision_resolved_here =
                !r.aliased && unaliased_name_counts[e.original_name] <= 1;
            const std::string permission_key = (never_contested || collision_resolved_here)
                                                    ? e.original_name
                                                    : r.stable_identity;

            ++surface_name_counts[r.surface_name];
            kind_resolved.push_back({r.stable_identity, r.surface_name, permission_key});
        }

        for (const auto& [surface, count] : surface_name_counts) {
            if (count > 1) result.fatal_collisions.push_back(surface);
        }

        for (const auto& [identity, count] : identity_counts) {
            if (count > 1) result.ambiguous_identities.push_back(identity);
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
