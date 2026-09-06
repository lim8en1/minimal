#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minimal/commands/command.hpp"

namespace minimal::commands {

// Source-agnostic by construction, like ToolRegistry: core-registered and
// plugin-registered commands arrive through Register() and are indistinguishable
// afterwards. `/help` lists live from Name()/Summary()/Usage() -- there is no
// config-directory manifest, because a command's value is the code it runs and a
// data-only manifest would be a shadow copy free to drift.
class CommandRegistry {
public:
    // A duplicate SURFACE name is not resolved here. This records it and moves
    // on; the collision check reads attributions() and refuses to start, naming
    // both plugins. Silently shadowing would make behaviour depend on plugin
    // load order -- see ARCHITECTURE.md "Name collisions and aliases".
    void Register(std::unique_ptr<Command> command);

    Command* Find(std::string_view name) const;

    // Every registered command, sorted by surface name -- what /help renders.
    std::vector<const Command*> All() const;

    std::size_t size() const { return commands_.size(); }

    // --- Attribution, for collision reporting and aliasing ------------------
    //
    // Identical to ToolRegistry's seam and for the same reason: Register() has
    // no idea who is calling, so the loader opens a scoped window around each
    // plugin's registration callback and everything arriving inside it is
    // attributed to that plugin.
    //
    // Also carries the ALIAS map, which ToolRegistry does not need: a command's
    // surface name is what the user types, and plugins.json may rename it.
    // Applied at Register() -- before the collision check -- so aliasing is
    // itself a valid way to resolve a collision.
    //
    // Not thread-safe, and does not need to be: plugin loading is
    // single-threaded and finishes before any session exists.
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    // qualified_key is always "plugin.original_name", so it is unambiguous even
    // before collisions are resolved.
    void SetAliases(std::unordered_map<std::string, std::string> aliases) {
        aliases_ = std::move(aliases);
    }

    struct Attribution {
        std::string plugin_name;    // empty for commands core registers itself
        std::string original_name;  // what the command called itself
        std::string surface_name;   // the alias if one applied, else original
    };
    const std::vector<Attribution>& attributions() const { return attributions_; }

    // Startup rollback for a plugin whose registration threw: removes every
    // command -- and its attribution entries -- attributed to `plugin_name`.
    // Valid ONLY before the startup collision check and before aliases are
    // applied (they run after ALL plugin loading, in main.cpp), when the map
    // is still keyed by original attribution names. First-wins means the map
    // slot holds the FIRST registrant of a surface name, so a colliding
    // surface is erased only when no surviving attribution still claims it.
    void RemoveByAttribution(const std::string& plugin_name);

private:
    std::unordered_map<std::string, std::unique_ptr<Command>> commands_;  // by surface name
    std::string attribution_;
    std::unordered_map<std::string, std::string> aliases_;  // "plugin.name" -> alias
    std::vector<Attribution> attributions_;
};

}  // namespace minimal::commands
