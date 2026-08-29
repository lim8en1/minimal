#include "minimal/tools/permission.hpp"

namespace minimal::tools {

PermissionMode PermissionPolicy::mode_for(const std::string& tool_name,
                                          ToolCategory category) const {
    if (auto it = per_tool_overrides.find(tool_name); it != per_tool_overrides.end()) {
        return it->second;
    }
    switch (category) {
        case ToolCategory::ReadOnly:  return read_only;
        case ToolCategory::FileWrite: return file_write;
        case ToolCategory::Execute:   return execute;
    }
    return PermissionMode::Ask;  // unreachable; fail closed rather than fall through
}

PermissionMode McpPermissionPolicy::mode_for(const std::string& server_name) const {
    if (auto it = per_server.find(server_name); it != per_server.end()) return it->second;
    return default_mode;
}

}  // namespace minimal::tools
