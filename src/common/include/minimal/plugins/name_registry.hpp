#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace minimal::plugins {

// The registration kinds subject to the collision/alias rules -- see
// ARCHITECTURE.md's "Name collisions and aliases".
//
// Services are their own namespace: a service name may repeat a tool or
// command name without colliding, and services are NOT user-aliasable (a
// service name is addressed by code through ServiceRegistry::Find, never by
// the model or the user, so command_aliases has nothing to rename).
// Hook points follow the same shape (addressed by hooks.json `on` and by code,
// never aliased), and live in their own namespace for the same reason.
//
// ViewId/ToolResultRenderer/StatusSegment/SyntaxGrammar are the CLIENT host's
// kinds (track E5): shared machinery, one collision rule, two loaders.
enum class NameKind {
    Tool,
    Command,
    ViewId,
    ToolResultRenderer,
    MessageRenderer,   // client: keyed by ChatRowKind (its own namespace, not ToolResultRenderer's)
    Service,
    StatusSegment,
    SyntaxGrammar,
    HookPoint
};

// Models the three-naming-levels rule independent of the actual
// ToolRegistry/CommandRegistry/plugin loader: stable identity never changes,
// surface name is what the model/user sees (alias if set, else the original
// name) and must be unique, permission key is the non-alias -- bare when this
// entry is the only one still claiming its original_name (i.e. it has no
// alias, and no OTHER un-aliased entry shares that name), qualified
// (stable_identity) otherwise. Aliasing away one half of a same-name
// collision therefore lets the other half keep its bare permission key: the
// aliased entry already has its own distinct surface identity and no longer
// needs to reserve the bare name.
class NameRegistry {
public:
    void Register(NameKind kind, std::string plugin_name, std::string original_name);

    // qualified_key is always "plugin.original_name". Applied before collision
    // checking, so aliasing is itself a valid way to resolve one.
    void SetAlias(NameKind kind, std::string qualified_key, std::string alias);

    struct ResolvedEntry {
        std::string stable_identity;  // "plugin.original_name", never changes
        std::string surface_name;     // alias if any, else original_name
        std::string permission_key;   // bare original_name, or stable_identity if aliased/ambiguous
    };

    struct Result {
        std::vector<ResolvedEntry> entries;
        std::vector<std::string> fatal_collisions;   // duplicate surface names
        std::vector<std::string> dangling_aliases;    // alias key naming nothing registered
    };

    Result Resolve() const;

private:
    struct Entry {
        std::string plugin_name;
        std::string original_name;
    };

    std::unordered_map<int, std::vector<Entry>> entries_;
    std::unordered_map<int, std::unordered_map<std::string, std::string>> aliases_;  // qualified_key -> alias
};

}  // namespace minimal::plugins
