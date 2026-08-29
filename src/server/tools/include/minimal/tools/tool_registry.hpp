#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minimal/llm/chat.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::tools {

// Tool-source-agnostic by construction: built-in, plugin-registered, and
// MCP-discovered tools all arrive through Register() and are indistinguishable
// afterwards. MCP tools are namespaced `mcp__<server>__<tool>` at registration,
// which is also how ToolExecutor routes their permission decision.
class ToolRegistry {
public:
    void Register(std::unique_ptr<Tool> tool);

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

private:
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

}  // namespace minimal::tools
