#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "minimal/tools/permission.hpp"

namespace minimal::tools {

enum class PermissionDecision { Allow, Deny };

struct PermissionRequest {
    std::string session_id;
    std::string tool_name;
    std::string summary;  // one-line, human-readable, for the prompt
    ToolCategory category = ToolCategory::Execute;
    nlohmann::json tool_input;
};

// Blocks the calling (session worker) thread until answered.
//
// That only works because of the three-thread-per-session topology: the gRPC
// handler thread keeps reading, so the response that unblocks this call can
// actually arrive. An implementation that blocked the reading thread would
// deadlock on the first gated tool call.
class PermissionChecker {
public:
    virtual ~PermissionChecker() = default;
    virtual PermissionDecision check(const PermissionRequest& request) = 0;
};

}  // namespace minimal::tools
