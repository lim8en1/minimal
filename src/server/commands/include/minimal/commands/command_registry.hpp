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

    // A command and the SURFACE name it is dispatchable under. The surface key
    // is what the user types and what Find() resolves; Name() is only what the
    // command called itself. Advertising Name() for an aliased command would
    // list a name every invocation of which answers "unknown command".
    struct Entry {
        std::string surface;
        const Command* command;
    };

    // Every registered command with its surface name, sorted by surface -- what
    // /help renders and what ListCommands advertises.
    std::vector<Entry> All() const;

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

// --- The effective catalog: the global registry plus a project's own ---------
//
// Resolves `name` against `project` first (if given and non-null), falling
// back to `global` -- the ONE precedence rule every dispatch path must agree
// on, project-scoped commands (ProjectCommandSource) included: a project
// command must never read as "unknown" to the reader thread's pre-check
// (SessionServiceImpl::DispatchCommand) while still running fine through
// Dispatch below, or vice versa. A project command may shadow a global
// PROMPT SHORTHAND of the same name (matching how a project's own
// agents/<name>.md shadows a global preset by name), but never a harness or
// LLM command: project commands are working-tree content, and a repo-shipped
// commands/secret.md taking over /secret would ship the API key the user
// typed to the model as a turn. See ProjectMayShadow.
//
// True when a project command may take the surface name `global_command`
// holds: nothing global holds it, or the holder is itself a prompt
// shorthand. The ONE rule both dispatch (FindEffective) and the catalog
// (ListCommands) apply, so what autocomplete shows is what runs.
bool ProjectMayShadow(const Command* global_command);

Command* FindEffective(const CommandRegistry& global, const CommandRegistry* project,
                       const std::string& name);

// Every command FindEffective can reach, sorted by surface name: the project
// entries that may shadow (ProjectMayShadow), then every global entry no
// project entry shadowed. What ListCommands advertises and /help lists, so
// the catalog never shows a command dispatch would not run.
std::vector<CommandRegistry::Entry> EffectiveCatalog(const CommandRegistry& global,
                                                     const CommandRegistry* project);

}  // namespace minimal::commands
