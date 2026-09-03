#pragma once

#include <string>

#include "minimal/tools/permission.hpp"

namespace minimal::config {

// Shared string<->enum vocabulary used by BOTH permissions.json (fallback.*,
// per_tool_overrides, mcp_default_mode) and agents/*.md (permission_mode.*,
// mcp_permissions) -- same underlying enums, so one converter rather than two
// copies that could drift.
tools::PermissionMode ParsePermissionMode(const std::string& value);        // "allow"|"ask"|"deny", else throws
tools::PermissionRunMode ParsePermissionRunMode(const std::string& value);  // "normal"|"bypass"|"allow-only", else throws

// Inverse of ParsePermissionRunMode -- same spellings, used to render the run
// mode back over the wire (e.g. session metadata's permission_mode field).
std::string ToString(tools::PermissionRunMode mode);

}  // namespace minimal::config
