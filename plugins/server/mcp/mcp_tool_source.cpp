#include "minimal/mcp/mcp_tool_source.hpp"

#include "minimal/mcp/naming.hpp"

namespace minimal::mcp {

std::optional<std::string> McpToolSource::Activate(const std::string& member, std::string& error) {
    // A preset naming a server that is configured but currently disabled
    // (SetEnabled / mcps.json) is an intent to use it -- auto-enable rather
    // than silently lose those tools. A failure here (only possible if the
    // name vanished between StatusOf and SetEnabled, which cannot happen
    // since nothing removes a handle post-construction) falls through to the
    // ordinary EnsureAlive failure below.
    const auto status = manager_.StatusOf(member);
    if (status.configured && !status.enabled) {
        std::string enable_error;
        if (manager_.SetEnabled(member, true, enable_error)) {
            logger_.info("auto-enabled MCP server '{}' required by a preset", member);
        }
    }
    if (!manager_.EnsureAlive(member, error)) return std::nullopt;
    return McpGroup(member);
}

}  // namespace minimal::mcp
