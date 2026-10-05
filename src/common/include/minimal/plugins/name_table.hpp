#pragma once

// One registration namespace, and the collision/alias rule every plugin host
// applies to it (ARCHITECTURE.md, "Name collisions and aliases").
//
// Kind-agnostic on purpose: the HOSTS own their vocabulary (the server's tools,
// commands, services and hook points; the client's views, renderers, status
// segments...) and the policy for each, holding one NameTable per namespace.
// What is shared is the mechanism -- surface names, collisions naming their
// claimants, dangling aliases, ambiguous identities -- so the two loaders
// cannot drift apart on it.
//
// Two policies:
//   kName -- a name the plugin chooses and a user or the model addresses
//            (a tool, a command, a view id). Aliasable: an alias renames the
//            entry's surface, and is the designed way to keep both sides of a
//            collision.
//   kSlot -- a key that is a contract with something outside the plugin (a
//            service name code looks up, a hook point, a renderer's payload
//            kind). Never aliased: renaming the key would detach the entry from
//            what it serves. A collision means two implementations of one slot,
//            resolved by disabling one plugin.
//
// Identity: "plugin.original_name" (the STABLE identity; core registrations
// have an empty plugin name, giving ".name"). Alias keys are stable
// identities.

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace minimal::plugins {

std::string StableIdentity(std::string_view plugin_name, std::string_view original_name);

class NameTable {
public:
    enum class Policy { kName, kSlot };

    explicit NameTable(Policy policy) : policy_(policy) {}

    Policy policy() const { return policy_; }

    // Warns (does not refuse) when `plugin_name` contains a dot, the identity
    // separator: ("a.b","c") and ("a","b.c") compose the same identity.
    // Resolve() reports the case where that actually happens.
    void Register(std::string plugin_name, std::string original_name);

    // Whether some registration's stable identity is `qualified_key`.
    bool Claims(std::string_view qualified_key) const;

    // kName only; ignored (with a warning) on a slot table. Applied before
    // collision checking, so aliasing is itself a way to resolve a collision.
    void SetAlias(std::string qualified_key, std::string alias);

    struct Entry {
        std::string plugin_name;
        std::string original_name;
        std::string stable_identity;
        std::string surface_name;  // the alias if one applies, else original_name
        bool aliased = false;
    };
    struct Collision {
        std::string surface;
        std::vector<std::string> claimants;  // plugin names, first-registration order, deduplicated
    };
    struct Result {
        std::vector<Entry> entries;                     // registration order
        std::vector<Collision> collisions;              // sorted by surface
        std::vector<std::string> dangling_aliases;      // sorted; SetAlias keys matching nothing
        std::vector<std::string> ambiguous_identities;  // sorted; composed by 2+ registrations
    };
    Result Resolve() const;

private:
    struct Registration {
        std::string plugin_name;
        std::string original_name;
        std::string stable_identity;
    };

    Policy policy_;
    std::vector<Registration> registrations_;
    std::unordered_set<std::string> identities_;
    std::unordered_map<std::string, std::string> aliases_;  // qualified key -> alias
};

// Routes each alias to EVERY kName table claiming its key (a plugin providing
// a tool and a command under one name aliases both). A key only slot tables
// claim lands in `targets_slot`; a key nothing claims in `dangling`. Both
// sorted.
struct AliasRouting {
    std::vector<std::string> dangling;
    std::vector<std::string> targets_slot;
};
AliasRouting RouteAliases(const std::map<std::string, std::string>& aliases,
                          std::span<NameTable* const> tables);

}  // namespace minimal::plugins
