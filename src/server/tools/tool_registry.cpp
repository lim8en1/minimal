#include "minimal/tools/tool_registry.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/mcp/naming.hpp"
#include "minimal/tools/aliased_tool.hpp"

namespace minimal::tools {

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    if (!tool) return;
    const std::string name = tool->spec().name;
    // The mcp__ namespace belongs to the MCP manager: ReplaceMcpTools is its
    // only writer, and the executor routes every mcp__-prefixed name through
    // the server-scoped permission chain (tools::mcp_server_of()). A PLUGIN
    // registering such a name -- "mcp__shell" -- would ride that chain and
    // inherit permission decisions for a server that does not exist, so
    // attributed registrations are refused. Core built-ins (and the test
    // harness, which registers fake MCP tools with empty attribution) keep
    // access: they are the process's own code, not loadable input.
    // DefinitionsFor()'s skip below stays as defense in depth.
    if (!attribution_.empty() && name.rfind(mcp::kMcpPrefix, 0) == 0) {
        common::DefaultLogger().warn(
            "plugin '{}': tool '{}' registration refused -- the '{}' prefix is reserved for "
            "MCP tools",
            attribution_, name, mcp::kMcpPrefix);
        return;
    }
    std::shared_ptr<Tool> shared = std::move(tool);
    // First registration owns the map slot, always -- before AND after alias
    // application. The post-alias branch once used insert_or_assign, which made
    // a late-registered duplicate (spawn_subagent registered at serve stage)
    // silently shadowed or been shadowed; first-wins matches the pre-alias rule
    // and parks the loser where the collision diagnostics can still see it.
    //
    // The attribution/owner parallel arrays are recorded under the same lock as
    // the map: push_backs outside it raced a concurrent Register's resize, and
    // every other access to these arrays already goes through mutex_.
    const std::lock_guard<std::shared_mutex> lock(mutex_);
    // Recorded even when the name is a duplicate: collision detection needs both
    // sides to name them in its diagnostic.
    attributions_.push_back(Attribution{attribution_, name});
    owners_.push_back(shared.get());
    if (tools_.count(name)) {
        shadowed_.push_back(Pending{attributions_.back(), std::move(shared)});
    } else {
        tools_.emplace(name, std::move(shared));
    }
}

void ToolRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // Pre-alias, pre-serving rollback only -- see the header. owners_ parallels
    // attributions_, so both compact together; tools_ and shadowed_ drop the
    // entries whose OBJECT (not name) belonged to the plugin, which stays
    // exact even across shadowed duplicate names.
    const std::lock_guard<std::shared_mutex> lock(mutex_);
    std::set<const Tool*> doomed;
    std::size_t out = 0;
    for (std::size_t i = 0; i < attributions_.size(); ++i) {
        if (attributions_[i].plugin_name == plugin_name) {
            doomed.insert(owners_[i]);
            continue;
        }
        attributions_[out] = std::move(attributions_[i]);
        owners_[out] = owners_[i];
        ++out;
    }
    attributions_.resize(out);
    owners_.resize(out);
    for (auto it = tools_.begin(); it != tools_.end();) {
        if (doomed.count(it->second.get()) != 0) it = tools_.erase(it);
        else ++it;
    }
    for (auto it = shadowed_.begin(); it != shadowed_.end();) {
        if (doomed.count(it->tool.get()) != 0) it = shadowed_.erase(it);
        else ++it;
    }
}

ToolRegistry::ConfigNamespaceReport ToolRegistry::ConfigNamespaces() const {
    // Startup-only consumption (the sanity check, before serving), but the
    // uniform locking rule stands: every access to the attribution parallel
    // arrays goes through mutex_.
    std::shared_lock<std::shared_mutex> lock(mutex_);
    ConfigNamespaceReport report;
    std::set<std::pair<std::string, std::string>> seen_collisions;  // {ns, second_plugin}
    for (std::size_t i = 0; i < attributions_.size(); ++i) {
        const Tool* owner = owners_[i];
        if (!owner) continue;
        const std::string_view ns = owner->config_namespace();
        if (ns.empty()) continue;
        const auto inserted =
            report.owners.emplace(std::string(ns), attributions_[i].plugin_name);
        if (!inserted.second && inserted.first->second != attributions_[i].plugin_name) {
            // Same-plugin duplicates are normal (one plugin, several tools, one
            // namespace); only a CROSS-plugin claim is a collision, and the
            // first two claimants name the diagnostic. Several tools of the
            // second plugin claiming the same namespace must not produce
            // duplicate findings.
            if (seen_collisions.emplace(std::string(ns), attributions_[i].plugin_name).second) {
                report.collisions.push_back(
                    {std::string(ns), inserted.first->second, attributions_[i].plugin_name});
            }
        }
    }
    return report;
}

std::vector<std::string> ToolRegistry::PermissionKeys() const {
    // Iterates tools_, not owners_/attributions_: those two parallel arrays
    // also carry shadowed duplicates (see ConfigNamespaces above), which never
    // serve and would misrepresent the live, addressable tool set. tools_ is
    // keyed by SURFACE name (post-ApplyAliases), so PermissionKeyForUnlocked
    // resolves exactly the vocabulary PermissionPolicy::mode_for() matches.
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string> keys;
    keys.reserve(tools_.size());
    for (const auto& [name, tool] : tools_) {
        if (mcp_owners_.count(name)) continue;
        keys.push_back(std::string(PermissionKeyForUnlocked(name)));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::vector<std::string> ToolRegistry::DotenvProtectedPermissionKeys() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string> keys;
    for (const auto& [name, tool] : tools_) {
        if (mcp_owners_.count(name)) continue;
        if (tool && tool->default_dotenv_protected()) {
            keys.push_back(std::string(PermissionKeyForUnlocked(name)));
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
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
        std::shared_ptr<Tool> detached;
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

std::shared_ptr<Tool> ToolRegistry::Find(std::string_view name) const {
    // The shared_ptr copy IS the lifetime guarantee: ToolExecutor::Run holds
    // it across the permission check and execute() itself, without holding
    // this lock, and remains safe even while a concurrent ReplaceMcpTools
    // removes the entry from the map under it.
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = tools_.find(std::string(name));
    return it == tools_.end() ? nullptr : it->second;
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
        if (name.rfind(mcp::kMcpPrefix, 0) == 0) continue;
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
        prefixes.push_back(std::string(mcp::kMcpPrefix) + server + std::string(mcp::kMcpSeparator));
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
    const std::string prefix = std::string(mcp::kMcpPrefix) + server_name + std::string(mcp::kMcpSeparator);
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Retire the server's previous set: out of the map. Matching is on the
    // RECORDING owner (the server that registered the tool), not the string
    // prefix and not the parsed name: both "mcp__a__" prefix-matching and
    // McpServerOf parsing would also erase every tool of server "a_" -- a
    // restart of "a" wiping a superset namespace. The owner map is the only
    // source of truth that survives ambiguous names. An executor already
    // holding the shared_ptr from Find() keeps the object alive for as long
    // as its invocation runs (bounded by the permission Ask plus the call
    // itself); when the last reference drops, the object destroys itself. No
    // retired list, no process-lifetime accounting.
    for (auto it = tools_.begin(); it != tools_.end();) {
        const auto owner = mcp_owners_.find(it->first);
        if (owner != mcp_owners_.end() && owner->second == server_name) {
            mcp_owners_.erase(owner);
            it = tools_.erase(it);
        } else {
            ++it;
        }
    }

    // First registration of a name wins within the batch, matching Register().
    // Refusals and duplicates were never Find-able, so no executor can hold
    // them: they destroy right here rather than parking anywhere.
    for (auto& tool : tools) {
        if (!tool) continue;
        const std::string name = tool->spec().name;
        if (name.rfind(prefix, 0) != 0) {
            continue;  // foreign namespace: refused, destroyed with the batch
        }
        std::shared_ptr<Tool> shared = std::move(tool);
        if (tools_.count(name)) {
            continue;  // duplicate within the batch: first wins
        }
        tools_.emplace(name, std::move(shared));
        mcp_owners_.emplace(name, server_name);
    }
}

}  // namespace minimal::tools
