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
    if (aliases_applied_ || !tools_.count(name)) {
        tools_.insert_or_assign(std::move(name), std::move(tool));
    } else {
        shadowed_.push_back(Pending{attributions_.back(), std::move(tool)});
    }
}

void ToolRegistry::ApplyAliases(const std::map<std::string, std::string>& aliases) {
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
    auto it = tools_.find(std::string(name));
    return it == tools_.end() ? nullptr : it->second.get();
}

std::vector<llm::ToolDefinition> ToolRegistry::DefinitionsFor(
        const std::vector<std::string>& allowed) const {
    const bool allow_all =
        std::find(allowed.begin(), allowed.end(), "*") != allowed.end();

    std::vector<llm::ToolDefinition> out;
    out.reserve(allow_all ? tools_.size() : allowed.size());

    for (const auto& [name, tool] : tools_) {
        // Filter on the PERMISSION key, not the map key: allowed_tools never
        // references an alias (ARCHITECTURE.md, "Name collisions and aliases"),
        // so an aliased tool is addressed by its original/qualified name while
        // the model sees the surface name.
        const std::string_view key = PermissionKeyFor(name);
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

void ToolRegistry::SetPermissionKeys(std::unordered_map<std::string, std::string> keys) {
    permission_keys_ = std::move(keys);
}

std::string_view ToolRegistry::PermissionKeyFor(std::string_view surface_name) const {
    if (auto it = permission_keys_.find(std::string(surface_name)); it != permission_keys_.end()) {
        return it->second;
    }
    return surface_name;
}

}  // namespace minimal::tools
