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

private:
    std::unordered_map<std::string, std::unique_ptr<Tool>> tools_;
};

}  // namespace minimal::tools
