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
// MCP-discovered tools all arrive through Register() and are indistinguishable
// afterwards. MCP tools are namespaced `mcp__<server>__<tool>` at registration,
// which is also how ToolExecutor routes their permission decision.
//
// Concurrency: startup registration (Register/ApplyAliases/SetPermissionKeys)
// is single-threaded and completes before any session exists. Once MCP tools
// can be REPLACED at runtime (a shared MCP server process restarting and
// re-discovering), reads from session workers overlap writes for the first
// time -- so all map access goes through the mutex below. Startup mutators
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

    Tool* Find(std::string_view name) const;

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

    // Definitions for the mcp__<server>__* tools of exactly these servers,
    // sorted like DefinitionsFor (one request prefix, one cache). The caller --
    // the agent loop -- passes the allowed-AND-alive set it resolved with the
    // McpClientManager; servers not in the list contribute nothing, which is
    // how a dead or de-scoped server's tools vanish from the model's view.
    std::vector<llm::ToolDefinition> DefinitionsForMcpServers(
        const std::vector<std::string>& servers) const;

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

    // Registers or replaces the tool set of one MCP server, atomically from the
    // readers' point of view: after this returns, Find/DefinitionsFor see
    // exactly the new set. Called at discovery time and again on re-discovery
    // (a server process restarted) -- the mcp__ namespacing cannot collide with
    // other sources, so this bypasses attribution, aliasing and the shadowed
    // list entirely.
    //
    // NEVER DESTROYS a replaced tool: ToolExecutor::Run holds the raw Tool*
    // from Find() across a blocking permission check and the execute() call
    // itself, so freeing the old object would be a use-after-free under a
    // concurrent replacement. Superseded objects are parked in a retired list
    // and destroyed only at registry teardown -- the same process-lifetime
    // ownership rule the plugin loader applies to its dlopen handles.
    //
    // A tool whose name is not `mcp__<server>__<something>` (checked by
    // prefix, not by reparsing) is refused: this entry point owns exactly one
    // server's namespace. Within the batch, the FIRST registration of a name
    // wins, matching Register()'s rule.
    void ReplaceMcpTools(const std::string& server_name,
                         std::vector<std::unique_ptr<Tool>> tools);

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

    struct Pending {
        Attribution attr;  // the attribution entry this object belongs to
        std::unique_ptr<Tool> tool;
    };

    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
    std::vector<Pending> shadowed_;  // duplicate names, held not destroyed
    std::string attribution_;
    std::vector<Attribution> attributions_;
    // Parallel to attributions_: the object each entry refers to. Raw because
    // the registry owns every registered tool for the process lifetime -- the
    // pointer is valid while the object sits in tools_ or shadowed_, which is
    // exactly when ApplyAliases needs it.
    std::vector<Tool*> owners_;
    std::unordered_map<std::string, std::string> permission_keys_;
    bool aliases_applied_ = false;

    // mutable: readers are const and run on session workers; startup and MCP
    // replacement are the writers.
    mutable std::shared_mutex mutex_;
    std::vector<std::unique_ptr<Tool>> retired_;  // superseded MCP tools, see ReplaceMcpTools
};

}  // namespace minimal::tools
