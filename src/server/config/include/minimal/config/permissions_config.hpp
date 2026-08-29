#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>

#include "minimal/tools/permission.hpp"

namespace minimal::config {

struct PermissionsConfig {
    tools::PermissionRunMode default_run_mode = tools::PermissionRunMode::Normal;
    // fallback.{read_only,file_write,execute,out_of_workdir} and the top-level
    // per_tool_overrides key both land here -- PermissionOverrideLayer already
    // has both shapes, so reusing it avoids a redundant struct even though the
    // two are JSON siblings, not nested, in permissions.json itself.
    tools::PermissionOverrideLayer global_layer;
    tools::PermissionMode mcp_default_mode = tools::PermissionMode::Ask;
};

// Pure. Throws on an empty-string key inside per_tool_overrides -- reserved as
// the category-only-resolution sentinel used by agent::BuildPermissionPolicy.
PermissionsConfig ParsePermissionsConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. Throws if the file is missing or isn't
// valid JSON.
PermissionsConfig LoadPermissionsConfig(const std::filesystem::path& path);

}  // namespace minimal::config
