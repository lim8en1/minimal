#include "minimal/tools/permission.hpp"

namespace minimal::tools {

PermissionMode PermissionPolicy::mode_for(std::string_view tool_name, ToolCategory category) const {
    if (auto it = per_tool_overrides.find(std::string(tool_name)); it != per_tool_overrides.end()) {
        return it->second;
    }
    switch (category) {
        case ToolCategory::ReadOnly:  return read_only;
        case ToolCategory::FileWrite: return file_write;
        case ToolCategory::Execute:   return execute;
    }
    return PermissionMode::Ask;  // unreachable; fail closed rather than fall through
}

PermissionMode McpPermissionPolicy::mode_for(std::string_view server_name) const {
    if (auto it = per_server.find(std::string(server_name)); it != per_server.end()) {
        return it->second;
    }
    return default_mode;
}

namespace {

std::optional<PermissionMode> layer_category_mode(const PermissionOverrideLayer& layer,
                                                   ToolCategory category) {
    switch (category) {
        case ToolCategory::ReadOnly:  return layer.read_only;
        case ToolCategory::FileWrite: return layer.file_write;
        case ToolCategory::Execute:   return layer.execute;
    }
    return std::nullopt;  // unreachable
}

}  // namespace

PermissionMode resolve_permission_chain(const std::string& tool_name, ToolCategory category,
                                         const PermissionOverrideLayer& preset,
                                         const PermissionOverrideLayer& global) {
    if (auto it = preset.per_tool_overrides.find(tool_name); it != preset.per_tool_overrides.end()) {
        return it->second;
    }
    if (auto mode = layer_category_mode(preset, category)) return *mode;
    if (auto it = global.per_tool_overrides.find(tool_name); it != global.per_tool_overrides.end()) {
        return it->second;
    }
    if (auto mode = layer_category_mode(global, category)) return *mode;

    static const PermissionPolicy hardcoded_default{};
    return hardcoded_default.mode_for(tool_name, category);
}

PermissionMode resolve_out_of_workdir_chain(const PermissionOverrideLayer& preset,
                                             const PermissionOverrideLayer& global) {
    if (preset.out_of_workdir) return *preset.out_of_workdir;
    if (global.out_of_workdir) return *global.out_of_workdir;
    return PermissionMode::Ask;
}

PermissionMode resolve_mcp_chain(const std::optional<PermissionMode>& preset_per_server,
                                  PermissionMode global_default_mode) {
    if (preset_per_server) return *preset_per_server;
    return global_default_mode;
}

}  // namespace minimal::tools
