#include "minimal/tools/tool_registry.hpp"

#include <algorithm>
#include <utility>

#include "minimal/tools/aliased_tool.hpp"

namespace minimal::tools {

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    if (!tool) return;
    const std::string name = tool->spec().name;
    // Recorded even when the name is a duplicate: collision detection needs both
    // sides to name them in its diagnostic.
    attributions_.push_back(Attribution{attribution_, name});
    owners_.push_back(tool.get());
    // First registration owns the map slot, always -- before AND after alias
    // application. The post-alias branch once used insert_or_assign, which made
    // a late-registered duplicate (spawn_subagent registered at serve stage)
    // silently shadowed or been shadowed; first-wins matches the pre-alias rule
    // and parks the loser where the collision diagnostics can still see it.
    const std::lock_guard<std::shared_mutex> lock(mutex_);
    if (tools_.count(name)) {
        shadowed_.push_back(Pending{attributions_.back(), std::move(tool)});
    } else {
        tools_.emplace(name, std::move(tool));
    }
}

void ToolRegistry::ApplyAliases(const std::map<std::string, std::string>& aliases) {
    // Startup-only: exclusive for the whole body. It manipulates tools_ and
    // shadowed_ directly and must not overlap a runtime MCP replacement.
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (std::size_t i = 0; i < attributions_.size(); ++i) {
        const auto& attr = attributions_[i];
        const auto alias = aliases.find(attr.plugin_name + "." + attr.tool_name);
        if (alias == aliases.end()) continue;  // no alias: stays under its original name

        // Detach the object this attribution refers to, from wherever it lives.
        std::unique_ptr<Tool> detached;
        const Tool* owner = owners_[i];
        if (auto map_it = tools_.find(attr.tool_name);
            map_it != tools_.end() && map_it->second.get() == owner) {
            detached = std::move(map_it->second);
            tools_.erase(map_it);
        } else {
            for (auto it = shadowed_.begin(); it != shadowed_.end(); ++it) {
                if (it->tool.get() == owner) {
                    detached = std::move(it->tool);
                    shadowed_.erase(it);
                    break;
                }
            }
        }
        if (!detached) continue;  // unreachable: the registry owns every registered tool

        // Surface already taken: put it back and let the collision check report
        // it -- a fatal outcome, so which object holds the slot is moot.
        if (tools_.count(alias->second)) {
            shadowed_.push_back(Pending{attr, std::move(detached)});
            continue;
        }
        tools_.emplace(alias->second, std::make_unique<AliasedTool>(std::move(detached), alias->second));
    }
    aliases_applied_ = true;
}

Tool* ToolRegistry::Find(std::string_view name) const {
    // The returned pointer is used by ToolExecutor::Run across the permission
    // check and execute() itself, WITHOUT holding this lock. That is sound
    // only because tool objects are never destroyed before teardown -- the
    // retired_ list in ReplaceMcpTools is what guarantees it, not this lock.
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = tools_.find(std::string(name));
    return it == tools_.end() ? nullptr : it->second.get();
}

std::vector<llm::ToolDefinition> ToolRegistry::DefinitionsFor(
        const std::vector<std::string>& allowed) const {
    const bool allow_all =
        std::find(allowed.begin(), allowed.end(), "*") != allowed.end();

    // One shared acquisition for the whole scan. The permission-key lookup
    // inside the loop goes through the unlocked helper: nesting a second
    // shared_lock on this mutex is undefined (a queued writer between the two
    // acquisitions deadlocks the reader).
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<llm::ToolDefinition> out;
    out.reserve(allow_all ? tools_.size() : allowed.size());

    for (const auto& [name, tool] : tools_) {
        // MCP tools are NEVER governed by allowed_tools -- they belong to the
        // separate server-scoped chain (allowed_mcp_servers + the manager's
        // live set) and are added by DefinitionsForMcpServers(). Letting "*"
        // sweep them in here would advertise a dead or de-scoped server's
        // tools to the model.
        if (name.rfind("mcp__", 0) == 0) continue;
        // Filter on the PERMISSION key, not the map key: allowed_tools never
        // references an alias (ARCHITECTURE.md, "Name collisions and aliases"),
        // so an aliased tool is addressed by its original/qualified name while
        // the model sees the surface name.
        const std::string_view key = PermissionKeyForUnlocked(name);
        if (!allow_all && std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            continue;
        }
        out.push_back(tool->spec().definition);
    }

    // Iteration order of an unordered_map is unspecified, so without this the
    // request prefix would differ run to run and defeat prompt caching.
    std::sort(out.begin(), out.end(),
              [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                  return a.name < b.name;
              });
    return out;
}

std::vector<llm::ToolDefinition> ToolRegistry::DefinitionsForMcpServers(
        const std::vector<std::string>& servers) const {
    if (servers.empty()) return {};
    std::vector<std::string> prefixes;
    prefixes.reserve(servers.size());
    for (const auto& server : servers) {
        prefixes.push_back("mcp__" + server + "__");
    }

    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<llm::ToolDefinition> out;
    for (const auto& [name, tool] : tools_) {
        for (const auto& prefix : prefixes) {
            if (name.rfind(prefix, 0) == 0) {
                out.push_back(tool->spec().definition);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                  return a.name < b.name;
              });
    return out;
}

void ToolRegistry::SetPermissionKeys(std::unordered_map<std::string, std::string> keys) {
    const std::lock_guard<std::shared_mutex> lock(mutex_);
    permission_keys_ = std::move(keys);
}

std::string_view ToolRegistry::PermissionKeyFor(std::string_view surface_name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return PermissionKeyForUnlocked(surface_name);
}

void ToolRegistry::ReplaceMcpTools(const std::string& server_name,
                                   std::vector<std::unique_ptr<Tool>> tools) {
    const std::string prefix = "mcp__" + server_name + "__";
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Retire the server's previous set: out of the map, into retired_ -- NOT
    // destroyed. A session worker may be inside execute() on one of them right
    // now (ToolExecutor holds the raw pointer without the lock); freeing it
    // here would be a use-after-free under a concurrent re-discovery.
    for (auto it = tools_.begin(); it != tools_.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            retired_.push_back(std::move(it->second));
            it = tools_.erase(it);
        } else {
            ++it;
        }
    }

    // First registration of a name wins within the batch, matching Register().
    for (auto& tool : tools) {
        if (!tool) continue;
        const std::string name = tool->spec().name;
        if (name.rfind(prefix, 0) != 0) {
            // Refused, not renamed: this entry point owns exactly one server's
            // namespace, and silently re-homing a foreign name would hide a
            // caller bug. Park it so it is still diagnosable at teardown.
            retired_.push_back(std::move(tool));
            continue;
        }
        if (tools_.count(name)) {
            retired_.push_back(std::move(tool));
        } else {
            tools_.emplace(name, std::move(tool));
        }
    }
}

}  // namespace minimal::tools
