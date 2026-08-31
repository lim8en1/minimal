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
enum class NameKind { Tool, Command, ViewId, ToolResultRenderer, Service, HookPoint };

// Models the three-naming-levels rule independent of the actual
// ToolRegistry/CommandRegistry/plugin loader: stable identity never changes,
// surface name is what the model/user sees (alias if set, else the original
// name) and must be unique, permission key is the non-alias -- bare when
// unambiguous, qualified when two plugins provide the same original name.
class NameRegistry {
public:
    void Register(NameKind kind, std::string plugin_name, std::string original_name);

    // qualified_key is always "plugin.original_name". Applied before collision
    // checking, so aliasing is itself a valid way to resolve one.
    void SetAlias(NameKind kind, std::string qualified_key, std::string alias);

    struct ResolvedEntry {
        std::string stable_identity;  // "plugin.original_name", never changes
        std::string surface_name;     // alias if any, else original_name
        std::string permission_key;   // bare original_name, or stable_identity if ambiguous
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
