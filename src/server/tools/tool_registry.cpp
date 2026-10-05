#include "minimal/tools/tool_registry.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/tools/aliased_tool.hpp"

namespace minimal::tools {

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    if (!tool) return;
    const std::string name = tool->spec().name;
    // A reserved namespace belongs to the plugin that reserved it: its tool
    // groups are published through ReplaceGroupTools, and its permission
    // model claims those names. ANOTHER plugin registering such a name --
    // "mcp__shell" -- would ride that model and inherit decisions meant for
    // a group that does not exist, so attributed registrations from anyone
    // but the owner are refused. Core built-ins (and the test harness, which
    // registers fake group tools with empty attribution) keep access: they
    // are the process's own code, not loadable input. DefinitionsFor()'s
    // skip below stays as defense in depth.
    if (!attribution_.empty()) {
        const std::lock_guard<std::shared_mutex> lock(mutex_);
        if (const auto* ns = ReservedNamespaceOfUnlocked(name);
            ns != nullptr && ns->owner != attribution_) {
            common::DefaultLogger().warn(
                "plugin '{}': tool '{}' registration refused -- the '{}' prefix is reserved by "
                "{}",
                attribution_, name, ns->prefix,
                ns->owner.empty() ? std::string("core") : "plugin '" + ns->owner + "'");
            return;
        }
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
    generation_.fetch_add(1, std::memory_order_acq_rel);  // see generation()
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
    generation_.fetch_add(1, std::memory_order_acq_rel);  // see generation()
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
        if (group_owners_.count(name)) continue;
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
        if (group_owners_.count(name)) continue;
        if (tool && tool->default_dotenv_protected()) {
            keys.push_back(std::string(PermissionKeyForUnlocked(name)));
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::map<std::string, std::string> ToolRegistry::ConfigAliasPermissionKeys() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::map<std::string, std::string> out;
    for (const auto& [name, tool] : tools_) {
        if (group_owners_.count(name) || !tool) continue;
        for (const auto& alias : tool->config_aliases()) {
            out.emplace(alias, std::string(PermissionKeyForUnlocked(name)));
        }
    }
    return out;
}

std::vector<std::string> ToolRegistry::NoPatternRulePermissionKeys() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::string> keys;
    for (const auto& [name, tool] : tools_) {
        if (group_owners_.count(name) || !tool || tool->accepts_pattern_rules()) continue;
        keys.push_back(std::string(PermissionKeyForUnlocked(name)));
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
}

std::map<std::string, std::vector<std::string>> ToolRegistry::PermissionKeysByPlugin() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    // The live key set, exactly as PermissionKeys() computes it. A plugin's
    // tool answers to "plugin.name" when name resolution qualified it (two
    // plugins provide the name), else to the bare name -- matching keys
    // rather than objects keeps an aliased (wrapped) tool attributed.
    std::set<std::string> live;
    for (const auto& [name, tool] : tools_) {
        if (group_owners_.count(name)) continue;
        live.emplace(PermissionKeyForUnlocked(name));
    }
    std::map<std::string, std::vector<std::string>> out;
    for (const auto& attr : attributions_) {
        if (attr.plugin_name.empty()) continue;  // core built-ins belong to no plugin
        auto& keys = out[attr.plugin_name];
        if (const auto qualified = attr.plugin_name + "." + attr.tool_name; live.count(qualified)) {
            keys.push_back(qualified);
        } else if (live.count(attr.tool_name)) {
            keys.push_back(attr.tool_name);
        }
    }
    for (auto& [plugin, keys] : out) {
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    }
    return out;
}

void ToolRegistry::ApplyAliases(const std::map<std::string, std::string>& aliases) {
    // Startup-only: exclusive for the whole body. It manipulates tools_ and
    // shadowed_ directly and must not overlap a runtime MCP replacement.
    std::unique_lock<std::shared_mutex> lock(mutex_);
    generation_.fetch_add(1, std::memory_order_acq_rel);  // see generation()
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
        // Reserved-namespace (tool group) names are NEVER governed by
        // allowed_tools -- they belong to the preset's tool_sources and the
        // source's live set, and are added by DefinitionsForGroups(). Letting
        // "*" sweep them in here would advertise a dead or de-scoped group's
        // tools to the model.
        if (ReservedNamespaceOfUnlocked(name) != nullptr) continue;
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

std::vector<llm::ToolDefinition> ToolRegistry::DefinitionsForGroups(
        const std::vector<std::string>& groups) const {
    if (groups.empty()) return {};
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<llm::ToolDefinition> out;
    // Matched on the RECORDING group, never on a name prefix: group "mcp__a__"
    // must not sweep in "mcp__a___x" of group "mcp__a_".
    for (const auto& [name, group] : group_owners_) {
        if (std::find(groups.begin(), groups.end(), group) == groups.end()) continue;
        if (const auto it = tools_.find(name); it != tools_.end()) {
            out.push_back(it->second->spec().definition);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                  return a.name < b.name;
              });
    return out;
}

bool ToolRegistry::ReserveNamespace(std::string prefix) {
    const std::lock_guard<std::shared_mutex> lock(mutex_);
    generation_.fetch_add(1, std::memory_order_acq_rel);  // see generation()
    if (prefix.empty()) return false;
    for (const auto& ns : namespaces_) {
        // Overlap either way is a collision: one prefix inside another would
        // let the inner owner's group names ride the outer owner's model.
        if (ns.prefix.rfind(prefix, 0) == 0 || prefix.rfind(ns.prefix, 0) == 0) {
            common::DefaultLogger().warn(
                "tool namespace '{}' refused for {} -- it overlaps '{}' reserved by {}", prefix,
                attribution_.empty() ? std::string("core") : "plugin '" + attribution_ + "'",
                ns.prefix, ns.owner.empty() ? std::string("core") : "plugin '" + ns.owner + "'");
            return false;
        }
    }
    namespaces_.push_back(ReservedNamespace{std::move(prefix), attribution_});
    return true;
}

bool ToolRegistry::IsReserved(std::string_view name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ReservedNamespaceOfUnlocked(name) != nullptr;
}

std::optional<std::string> ToolRegistry::ReservedOwnerOf(std::string_view name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto* ns = ReservedNamespaceOfUnlocked(name);
    return ns != nullptr ? std::optional<std::string>(ns->owner) : std::nullopt;
}

const ToolRegistry::ReservedNamespace* ToolRegistry::ReservedNamespaceOfUnlocked(
    std::string_view name) const {
    for (const auto& ns : namespaces_) {
        if (name.rfind(ns.prefix, 0) == 0) return &ns;
    }
    return nullptr;
}

void ToolRegistry::SetPermissionKeys(std::unordered_map<std::string, std::string> keys) {
    const std::lock_guard<std::shared_mutex> lock(mutex_);
    generation_.fetch_add(1, std::memory_order_acq_rel);  // see generation()
    permission_keys_ = std::move(keys);
}

std::string_view ToolRegistry::PermissionKeyFor(std::string_view surface_name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return PermissionKeyForUnlocked(surface_name);
}

void ToolRegistry::ReplaceGroupTools(const std::string& group,
                                     std::vector<std::unique_ptr<Tool>> tools) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    generation_.fetch_add(1, std::memory_order_acq_rel);  // see generation()
    // A group lives inside a reserved namespace, or it is not a group: an
    // unreserved prefix would let its names collide with Register()ed tools.
    if (group.empty() || ReservedNamespaceOfUnlocked(group) == nullptr) {
        common::DefaultLogger().warn(
            "tool group '{}' refused -- it is not inside a reserved tool namespace", group);
        return;
    }

    // Retire the group's previous set: out of the map. Matching is on the
    // RECORDING group, not the string prefix: prefix-matching "mcp__a__"
    // would also erase every tool of group "mcp__a_" ("mcp__a___x") -- a
    // restart of "a" wiping a superset namespace. The owner map is the only
    // source of truth that survives ambiguous names. An executor already
    // holding the shared_ptr from Find() keeps the object alive for as long
    // as its invocation runs (bounded by the permission Ask plus the call
    // itself); when the last reference drops, the object destroys itself. No
    // retired list, no process-lifetime accounting.
    for (auto it = tools_.begin(); it != tools_.end();) {
        const auto owner = group_owners_.find(it->first);
        if (owner != group_owners_.end() && owner->second == group) {
            group_owners_.erase(owner);
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
        if (name.rfind(group, 0) != 0 || name.size() == group.size()) {
            continue;  // outside this group's prefix: refused, destroyed with the batch
        }
        std::shared_ptr<Tool> shared = std::move(tool);
        if (tools_.count(name)) {
            continue;  // duplicate within the batch (or another group's name): first wins
        }
        tools_.emplace(name, std::move(shared));
        group_owners_.emplace(name, group);
    }
}

}  // namespace minimal::tools
