#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minimal/llm/chat.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools {

// Tool-source-agnostic by construction: built-in, plugin-registered, and
// dynamically discovered tools all end up here indistinguishably. Register()
// takes built-ins and plugin tools. DYNAMIC tools (a plugin's tool source --
// the MCP plugin's servers) live in a namespace the plugin RESERVED
// (ReserveNamespace, e.g. "mcp__"), grouped per member ("mcp__<server>__"),
// and arrive ONLY through ReplaceGroupTools(). Register() refuses names in
// another plugin's reserved namespace (core and the test harness register
// unattributed).
//
// Concurrency: startup registration (Register/ApplyAliases/SetPermissionKeys/
// ReserveNamespace) is single-threaded and completes before any session
// exists. Once group tools can be REPLACED at runtime (a server process
// restarting and re-discovering), reads from session workers overlap writes
// for the first time -- so all map access goes through the mutex below. Startup mutators
// take the exclusive lock too, serially and before serving, which costs
// nothing and keeps the rules uniform.
class ToolRegistry {
public:
    // A duplicate name does NOT overwrite: the first registration keeps the map
    // slot, the later one is held in a shadowed list. Destroying it here -- as
    // insert_or_assign did -- made a collision silent and unfixable by aliasing:
    // by the time an alias could rename the second registrant, the first object
    // was already gone. Shadowed tools are never served (the collision is fatal
    // upstream) but they survive long enough to be renamed or diagnosed.
    void Register(std::unique_ptr<Tool> tool);

    // The second half of startup registration, called ONCE after every plugin
    // and core built-in has registered, before any session exists: resolves each
    // aliased tool's surface name (its plugins.json alias, else its original)
    // and re-keys it in the map, wrapping it so spec() advertises the surface.
    // A tool whose surface is already taken stays shadowed -- ResolveRegistrationNames()
    // reports that as a fatal collision and the process exits before serving, so
    // which object holds the slot is moot. Late registrations after this call
    // (MCP tools) insert directly: their mcp__ namespacing cannot collide and no
    // alias applies to them.
    void ApplyAliases(const std::map<std::string, std::string>& aliases);

    // Registry-wide ownership is std::shared_ptr: ToolExecutor::Run holds the
    // Find() result across a blocking permission check and the execute() call
    // itself -- a permission Ask can wait minutes on a human -- while a
    // concurrent MCP replacement swaps the map. The shared reference is what
    // keeps an in-flight tool object alive; there is no retired list to reason
    // about.
    std::shared_ptr<Tool> Find(std::string_view name) const;

    // Returns definitions for the allowed subset, SORTED BY NAME.
    //
    // The sort is not cosmetic. `tools` sits at the very front of the serialized
    // prefix providers cache on, and this is rebuilt every turn (MCP liveness
    // and /agent can change the set), so unstable ordering would silently
    // destroy prompt-cache hits on every turn -- visible only as cost and
    // latency, never as an error.
    //
    // `allowed` containing a single "*" means every registered tool.
    std::vector<llm::ToolDefinition> DefinitionsFor(const std::vector<std::string>& allowed) const;

    // Definitions for the tools of exactly these groups, sorted like
    // DefinitionsFor (one request prefix, one cache). The caller -- the agent
    // loop -- passes the allowed-AND-active groups its session resolved
    // through the tool sources; groups not in the list contribute nothing,
    // which is how a dead or de-scoped member's tools vanish from the
    // model's view.
    std::vector<llm::ToolDefinition> DefinitionsForGroups(
        const std::vector<std::string>& groups) const;

    // --- Reserved namespaces and tool groups --------------------------------
    //
    // Reserves a tool-name prefix for the attributed plugin (or core, when
    // unattributed): only its own ReplaceGroupTools may publish names there,
    // Register() refuses them from anyone else, and DefinitionsFor() never
    // lists them. Startup only. False (logged) for an empty prefix or one
    // overlapping an existing reservation in either direction.
    bool ReserveNamespace(std::string prefix);

    // True when `name` falls inside any reserved namespace.
    bool IsReserved(std::string_view name) const;

    std::size_t size() const { return tools_.size(); }

    // --- Attribution, for collision reporting -------------------------------
    //
    // NameRegistry's three-naming-levels rule needs (plugin, original_name)
    // pairs, but Register() has no idea who is calling. The loader opens a
    // scoped window around each plugin's registration callback so every name
    // that arrives inside it is attributed to that plugin.
    //
    // This is deliberately not thread-safe, and does not need to be: plugin
    // loading is single-threaded and completes before any session exists. The
    // pattern would be unsound anywhere later in the process lifetime.
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    struct Attribution {
        std::string plugin_name;  // empty for built-ins registered by core itself
        std::string tool_name;
    };
    const std::vector<Attribution>& attributions() const { return attributions_; }

    // --- Per-preset tool_config namespaces ----------------------------------
    //
    // What config_namespace() each registered (non-MCP) tool claims, with the
    // plugin that claimed it. The sanity check consumes both halves: owners for
    // the "unclaimed tool_config key" warning, collisions for the fatal
    // "two plugins claim one namespace" -- the NameRegistry discipline one
    // level up. Multiple tools of ONE plugin sharing a namespace is normal
    // (deduped here); a namespace claimed by a core built-in (empty plugin
    // attribution) is likewise folded under "".
    struct ConfigNamespaceCollision {
        std::string ns;
        std::string first_plugin;
        std::string second_plugin;
    };
    struct ConfigNamespaceReport {
        std::map<std::string, std::string> owners;  // namespace -> first claiming plugin
        std::vector<ConfigNamespaceCollision> collisions;
    };
    ConfigNamespaceReport ConfigNamespaces() const;

    // --- Structural tool-name queries ---------------------------------------
    //
    // Every LIVE (non-shadowed, non-MCP) tool's permission key, sorted and
    // deduped. For callers that need "is this string an actual registered
    // tool" structurally -- a renamed/aliased/third-party tool answers
    // correctly here, unlike a fixed literal name list.
    std::vector<std::string> PermissionKeys() const;

    // The subset of PermissionKeys() whose tool opted into
    // Tool::default_dotenv_protected().
    std::vector<std::string> DotenvProtectedPermissionKeys() const;

    // Startup rollback for a plugin whose registration threw: removes every
    // tool -- and its attribution/owner entries -- attributed to `plugin_name`.
    // Valid ONLY before ApplyAliases and before any session exists: aliases
    // re-key the map by surface name, so attribution matching (original names)
    // is only sound before it runs, and no executor can hold a Find() result
    // yet, so dropping the shared_ptr here frees the tool outright.
    void RemoveByAttribution(const std::string& plugin_name);

    // --- Permission keys ------------------------------------------------------
    //
    // surface_name -> the NON-alias key per_tool_overrides / allowed_tools may
    // reference (bare original name when unambiguous, "plugin.original_name"
    // when two plugins provide it -- see NameRegistry). Populated once at
    // startup from ResolveRegistrationNames(), before any session exists.
    void SetPermissionKeys(std::unordered_map<std::string, std::string> keys);

    // Falls back to the name itself when no resolution entry exists: that is the
    // case for MCP tools (registered after startup) and, for every unaliased
    // tool, the key IS the name -- so the fallback is correct, not accidental.
    std::string_view PermissionKeyFor(std::string_view surface_name) const;

    // Registers or replaces the tool set of one GROUP -- a name prefix inside
    // a reserved namespace ("mcp__<server>__") -- atomically from the
    // readers' point of view: after this returns, Find/DefinitionsForGroups
    // see exactly the new set. Called at discovery time and again on
    // re-discovery (a server process restarted); reserved namespacing cannot
    // collide with Register()ed tools, so this bypasses attribution, aliasing
    // and the shadowed list entirely. A group outside every reserved
    // namespace is refused (logged).
    //
    // Replaced tools are NOT freed here: they leave the map, but an executor
    // that already holds the shared_ptr from Find() keeps the object alive
    // until its invocation finishes. A tool whose name is not under `group`
    // is refused -- destroyed outright, since it was never Find-able and no
    // executor can hold it: this entry point owns exactly one group. Within
    // the batch, the FIRST registration of a name wins, matching Register()'s
    // rule.
    void ReplaceGroupTools(const std::string& group, std::vector<std::unique_ptr<Tool>> tools);

private:
    // Locking discipline: every tools_/permission_keys_ access holds mutex_.
    // This helper is the lock-free inner half of PermissionKeyFor, so
    // DefinitionsFor can scan under ONE shared acquisition instead of nesting
    // a second one (undefined for shared_mutex).
    std::string_view PermissionKeyForUnlocked(std::string_view surface_name) const {
        if (auto it = permission_keys_.find(std::string(surface_name)); it != permission_keys_.end()) {
            return it->second;
        }
        return surface_name;
    }

    struct ReservedNamespace {
        std::string prefix;
        std::string owner;  // plugin name; empty for core
    };
    const ReservedNamespace* ReservedNamespaceOfUnlocked(std::string_view name) const;

    struct Pending {
        Attribution attr;  // the attribution entry this object belongs to
        std::shared_ptr<Tool> tool;
    };

    std::unordered_map<std::string, std::shared_ptr<Tool>> tools_;
    std::vector<Pending> shadowed_;  // duplicate names, held not destroyed
    std::string attribution_;
    std::vector<Attribution> attributions_;
    // Parallel to attributions_: the object each entry refers to. Raw and
    // non-owning: ApplyAliases only matches against tools still in tools_ or
    // shadowed_, which keep them alive. Group tools never register through
    // Register(), so these never dangle via ReplaceGroupTools (which erases
    // only group-owned names, none of which are attributed).
    std::vector<Tool*> owners_;
    std::unordered_map<std::string, std::string> permission_keys_;
    // Tool name -> the group that published it. The authority for
    // ReplaceGroupTools retirement and DefinitionsForGroups: parsed names are
    // ambiguous ("a_" vs "a" + "_x" tool), prefixes wipe superset groups, but
    // the recording group is exact. Guarded by mutex_.
    std::unordered_map<std::string, std::string> group_owners_;
    std::vector<ReservedNamespace> namespaces_;  // guarded by mutex_
    bool aliases_applied_ = false;

    // mutable: readers are const and run on session workers; startup and MCP
    // replacement are the writers.
    mutable std::shared_mutex mutex_;
};

}  // namespace minimal::tools
