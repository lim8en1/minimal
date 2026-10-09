#include "minimal/mcp/mcp_tool_source.hpp"

namespace minimal::mcp {

bool McpToolSource::Activate(const std::string& member, std::string& error) {
    // A disabled server (mcps.json / SetEnabled) stays disabled even when a
    // preset names it, and that is not a failure: the session (or subagent)
    // starts with its group empty, and /mcp <name> enable connects it and
    // fills the group live. Enabling is an explicit user action.
    const auto status = manager_.StatusOf(member);
    if (status.configured && !status.enabled) {
        logger_.info("MCP server '{}' is disabled; its tools appear when it is enabled", member);
        return true;
    }
    return manager_.EnsureAlive(member, error);
}

}  // namespace minimal::mcp
