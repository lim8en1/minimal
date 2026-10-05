#pragma once

// "core.permission_gate": the session permission gate, for a plugin that needs
// to gate something other than a tool call (a resource read, an outbound
// action) for a given session. The plugin's own permission model decides
// (PermissionModel::ResolveSubject, consulted for `check.owner` -- pass your
// plugin's name); core composes the operator's denies, the subagent ceiling,
// run mode, session grants and the human prompt around it
// (ToolExecutor::CheckSubject).
//
// Blocking: the check runs on the session's worker, so from another thread it
// answers between turns. Refused from inside a tool call (use the tool's own
// permission instead), for an unknown session, and before serving starts.

#include <atomic>
#include <string>

#include "minimal/tools/tool_executor.hpp"

namespace minimal::agent {

inline constexpr const char* kPermissionGateServiceName = "core.permission_gate";

class PermissionGateService {
public:
    virtual ~PermissionGateService() = default;
    virtual tools::ToolExecutor::SubjectVerdict Check(const std::string& session_id,
                                                      tools::ToolExecutor::SubjectCheck check) = 0;
};

class SessionRegistry;

// The host's implementation: provided before plugins load (services seal
// then), pointed at the session registry once serving starts.
class SessionPermissionGate final : public PermissionGateService {
public:
    void SetRegistry(SessionRegistry* registry) { registry_.store(registry); }
    tools::ToolExecutor::SubjectVerdict Check(const std::string& session_id,
                                              tools::ToolExecutor::SubjectCheck check) override;

private:
    std::atomic<SessionRegistry*> registry_{nullptr};
};

}  // namespace minimal::agent
