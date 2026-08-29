#pragma once

#include <unordered_map>
#include <string>

#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// Flattens a preset's + permissions.json's PermissionOverrideLayer into the
// flat tools::PermissionPolicy that ToolExecutor::mode_for() consumes at call
// time. Pure, no I/O -- the actual glue gap between the pure precedence-chain
// math in permission.hpp and a real config file.
//
// Deliberate simplification: the true precedence is preset-per-tool >
// preset-category > global-per-tool > global-category, but replicating that
// exactly for a tool named ONLY in `global.per_tool_overrides` would require
// knowing that tool's ToolCategory -- unavailable here, since tools are
// plugin-registered and presets must resolve without consulting ToolRegistry.
// This instead treats any explicit per-tool rule (either layer) as outranking
// a preset's blanket category default. The two diverge only when a preset sets
// a category default, deliberately omits a specific tool from its own
// overrides, AND permissions.json separately pins that same tool -- an unusual
// combination -- and the deviation errs toward keeping the explicit global
// rule in force rather than silently losing it to a blanket default.
tools::PermissionPolicy BuildPermissionPolicy(const tools::PermissionOverrideLayer& preset,
                                               const tools::PermissionOverrideLayer& global);

// Exact -- McpPermissionPolicy's own two-layer shape already matches
// resolve_mcp_chain() once per_server/default_mode are seeded this way.
tools::McpPermissionPolicy BuildMcpPermissionPolicy(
    const std::unordered_map<std::string, tools::PermissionMode>& preset_mcp_permissions,
    tools::PermissionMode global_mcp_default_mode);

}  // namespace minimal::agent
