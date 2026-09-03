#pragma once

#include <string>
#include <string_view>

namespace minimal::mcp {

// MCP tools surface to the model as `mcp__<server>__<tool>` -- the namespacing
// that keeps them collision-free with built-ins and across servers, and how
// tools::mcp_server_of() recovers the owning server for the server-scoped
// permission chain.
inline constexpr std::string_view kMcpPrefix = "mcp__";
inline constexpr std::string_view kMcpSeparator = "__";

// True when `server_name` can take part in that mangling unambiguously:
// non-empty, no `__` (it would make the wire name ambiguous against the
// separator), no whitespace or control characters (spaces would read as two
// words in the model-facing name; controls would corrupt logging).
bool IsValidMcpServerName(std::string_view server_name);

// Same rule for tool names reported by a remote server. Unlike server names,
// these cannot be fixed in config -- a remote tool whose name contains `__`
// is skipped at registration rather than mangling the namespace.
bool IsValidMcpToolName(std::string_view tool_name);

// Preconditions: both names valid. Not checked here -- the caller validates
// first and decides its own failure mode (config error vs. skipped tool).
std::string McpToolName(std::string_view server_name, std::string_view tool_name);

// Inverse of McpToolName: recovers the server segment from a wire name, or
// empty if `tool_name` is not one of ours (missing prefix, or no separator
// after it). Used by the server-scoped permission chain (tools::mcp_server_of()).
std::string McpServerOf(std::string_view tool_name);

}  // namespace minimal::mcp
