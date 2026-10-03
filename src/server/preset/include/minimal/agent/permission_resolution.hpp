#pragma once

#include <unordered_map>
#include <string>
#include <vector>

#include "minimal/tools/permission.hpp"

namespace minimal::agent {

// Builds the tools::PermissionPolicy that ToolExecutor consults at call time
// from a preset's and permissions.json's PermissionOverrideLayer. Pure, no
// I/O. Tool/category modes are NOT flattened: the policy keeps both layers
// (PermissionPolicy::layers) and resolves them per call, when the tool's
// category is known -- presets must resolve without consulting ToolRegistry,
// and a flat merge without the category had to guess a precedence (and let a
// global `bash: allow` beat plan's `execute: deny`). The precedence is
// documented on PermissionPolicy::layers. Patterns, out_of_workdir and
// network are merged here.
//
// `preset_is_trusted`: false for a preset resolved from PROJECT-LOCAL
// discovery (a repo-shipped agents/*.md) -- see ProjectDirs. A project file is
// content a session's working directory supplied, not something the operator
// configured, so it must never WIDEN what permissions.json already allows: a
// clone-and-run of a hostile repo shipping `permission: {bash: allow}` would
// otherwise silently replace the operator's Ask/Deny floor with no prompt, no
// diff, no trust decision. Untrusted mode resolves every call as if the preset
// were absent and folds the preset's own opinion (per-tool rule, else
// category) over it with most_restrictive, so a project preset may only
// narrow; it contributes no pattern rules (their effect cannot be bounded
// here). A GLOBAL-config preset (the operator's own agents/*.md) stays fully
// trusted.
// `dotenv_protected_permission_keys`: the live ToolRegistry's
// DotenvProtectedPermissionKeys() -- what tools::default_pattern_rules()
// applies its dotenv-deny defaults to. Defaulted to {} (no dotenv defaults)
// so callers that do not have a ToolRegistry handy (most tests) keep
// compiling; this layer still never consults ToolRegistry directly (see
// above) -- the caller resolves the list once, elsewhere, and hands it down.
tools::PermissionPolicy BuildPermissionPolicy(
    const tools::PermissionOverrideLayer& preset, const tools::PermissionOverrideLayer& global,
    bool preset_is_trusted = true,
    const std::vector<std::string>& dotenv_protected_permission_keys = {});

}  // namespace minimal::agent
