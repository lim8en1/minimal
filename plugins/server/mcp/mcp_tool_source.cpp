#include "minimal/mcp/mcp_tool_source.hpp"

#include "minimal/mcp/naming.hpp"

namespace minimal::mcp {

std::optional<std::string> McpToolSource::Activate(const std::string& member, std::string& error) {
    // A disabled server (mcps.json / SetEnabled) stays disabled even when a
    // preset names it: EnsureAlive reports "is disabled" without connecting,
    // and the session carries on without those tools. Enabling is an explicit
    // user action (/mcp enable).
    const auto status = manager_.StatusOf(member);
    if (status.configured && !status.enabled) {
        logger_.info("MCP server '{}' is disabled; skipping", member);
    }
    if (!manager_.EnsureAlive(member, error)) return std::nullopt;
    return McpGroup(member);
}

}  // namespace minimal::mcp
