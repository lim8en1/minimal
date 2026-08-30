#pragma once

#include <string>
#include <vector>

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
    // What this invocation reaches, and which session grants may be offered.
    // Empty grants means allow-once/deny are the only choices.
    PermissionDisclosure disclosure;
};

// A decision, plus any grants the operator chose to remember for this session.
// Returning grants here rather than mutating shared state keeps the checker (and
// therefore the whole client round-trip) free of the grant store: ToolExecutor
// owns it, so the "what is remembered" rule lives in one place.
struct PermissionResponse {
    PermissionDecision decision = PermissionDecision::Deny;
    std::vector<std::string> granted_keys;  // must be keys from request.disclosure
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
    virtual PermissionResponse check(const PermissionRequest& request) = 0;
};

}  // namespace minimal::tools
