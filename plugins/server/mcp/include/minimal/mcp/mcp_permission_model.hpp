#pragma once

// The MCP permission model (tools/permission_model.hpp): claims every
// mcp__<server>__<tool> name and replaces core's base stage with the server's
// mode refined by the operator's per-tool rules on the full name. Core folds
// everything else around it (per-tool deny kill switch, subagent ceiling,
// run modes, grants, advisor, the human).
//
// Slices, keyed by this plugin's name ("mcp"):
//   permissions.json   plugins.mcp            {"default": "<mode>",
//                                              "transport": {"local": "<mode>", "remote": "<mode>"},
//                                              "signals": {"<channel>/<signal>|<channel>|*": "<mode>"}}
//   preset frontmatter plugin_permissions.mcp {"servers": {"<server>": "<mode>"},
//                                              "signals": {...}}
// A server's mode: its preset rule, else its transport's (local = stdio,
// remote = http), else the default. A channel signal (ResolveSubject, kind
// "signal") resolves by its own key, then its channel, then "*"; no rule =
// delivered (the subscription consented).

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "minimal/tools/permission.hpp"
#include "minimal/tools/permission_model.hpp"

namespace minimal::mcp {

struct McpPermissionPolicy {
    std::unordered_map<std::string, tools::PermissionMode> per_server;
    std::unordered_map<std::string, tools::PermissionMode> per_transport;  // "local"/"remote"
    std::unordered_map<std::string, tools::PermissionMode> signals;
    tools::PermissionMode default_mode = tools::PermissionMode::Ask;

    // `locality` ("local"/"remote") when known.
    tools::PermissionMode mode_for(std::string_view server_name,
                                   std::optional<std::string_view> locality = std::nullopt) const;
    // nullopt: no rule for this signal.
    std::optional<tools::PermissionMode> signal_mode(std::string_view channel,
                                                     std::string_view signal) const;
};

// Seeds an McpPermissionPolicy from its two config layers. For an UNTRUSTED
// (project-local) preset each per-server mode is folded with
// most_restrictive() against the global default, so the preset may only
// narrow -- `{server: allow}` under an Ask/Deny global floor cannot
// auto-approve that server's calls.
McpPermissionPolicy BuildMcpPolicy(
    const std::unordered_map<std::string, tools::PermissionMode>& preset_per_server,
    tools::PermissionMode global_default_mode, bool preset_trusted);

// One MCP tool's mode: the server's mode refined by a per-tool rule keyed by
// the full tool name (`mcp__<server>__<tool>`) in `policy`'s per-tool rules --
// so `get_issue` can be allowed while `delete_repo` asks. Same shape as a
// built-in's tool-vs-category precedence: a preset per-tool rule wins; a
// global per-tool rule and a preset per-server rule that both speak fold
// most-restrictively; a global per-tool rule beats the global default; an
// untrusted preset only narrows.
tools::PermissionMode ResolveMcpToolMode(const tools::PermissionPolicy& policy,
                                         const McpPermissionPolicy& mcp_policy,
                                         std::string_view server_name,
                                         std::string_view tool_name,
                                         std::string* reason = nullptr,
                                         std::optional<std::string_view> locality = std::nullopt);

// `locality_of` answers a server's "local"/"remote" (the plugin passes its
// manager's); without one the transport dimension never applies.
using McpLocalityFn = std::function<std::optional<std::string>(std::string_view server)>;
std::shared_ptr<const tools::PermissionModel> MakeMcpPermissionModel(McpLocalityFn locality_of = {});

// This plugin's registry key: the model's name, and its slices' key.
inline constexpr std::string_view kMcpPluginName = "mcp";

// Binds the model to an already-built policy -- for tests and callers that
// hold an McpPermissionPolicy directly.
tools::BoundPermissionModel BindMcpPermissionModel(McpPermissionPolicy policy);

}  // namespace minimal::mcp
