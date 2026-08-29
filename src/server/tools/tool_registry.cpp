#include "minimal/tools/tool_registry.hpp"

#include <algorithm>
#include <utility>

namespace minimal::tools {

void ToolRegistry::Register(std::unique_ptr<Tool> tool) {
    if (!tool) return;
    std::string name = tool->spec().name;
    // Recorded even when the name is a duplicate: the loader needs both sides of
    // a collision to name them in its diagnostic.
    attributions_.push_back(Attribution{attribution_, name});
    tools_.insert_or_assign(std::move(name), std::move(tool));
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
        if (!allow_all && std::find(allowed.begin(), allowed.end(), name) == allowed.end()) {
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

}  // namespace minimal::tools
