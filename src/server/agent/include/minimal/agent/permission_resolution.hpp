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
//
// `preset_is_trusted`: false for a preset resolved from PROJECT-LOCAL
// discovery (a repo-shipped agents/*.md) -- see ProjectDirs. A project file is
// content a session's working directory supplied, not something the operator
// configured, so it must never WIDEN what permissions.json already allows: a
// clone-and-run of a hostile repo shipping `permission: {bash: allow}` would
// otherwise silently replace the operator's Ask/Deny floor with no prompt, no
// diff, no trust decision. Untrusted mode folds each dimension against what
// global alone would have resolved to (most_restrictive of "with preset" and
// "without preset"), so a project preset may only narrow. Untrusted per-tool
// overrides may only set AlwaysDeny for exactly the same reason -- Ask/Allow
// entries are dropped rather than applied, since evaluating whether they would
// still be a narrowing for an arbitrary tool name would require the tool's
// ToolCategory, unavailable here. A GLOBAL-config preset (the operator's own
// agents/*.md) stays fully trusted, as before.
tools::PermissionPolicy BuildPermissionPolicy(const tools::PermissionOverrideLayer& preset,
                                               const tools::PermissionOverrideLayer& global,
                                               bool preset_is_trusted = true);

// Exact -- McpPermissionPolicy's own two-layer shape already matches
// resolve_mcp_chain() once per_server/default_mode are seeded this way.
tools::McpPermissionPolicy BuildMcpPermissionPolicy(
    const std::unordered_map<std::string, tools::PermissionMode>& preset_mcp_permissions,
    tools::PermissionMode global_mcp_default_mode);

}  // namespace minimal::agent
