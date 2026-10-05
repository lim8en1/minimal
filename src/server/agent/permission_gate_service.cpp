#include "minimal/agent/permission_gate_service.hpp"

#include <utility>

#include "minimal/agent/session.hpp"
#include "minimal/agent/session_registry.hpp"

namespace minimal::agent {

tools::ToolExecutor::SubjectVerdict SessionPermissionGate::Check(
    const std::string& session_id, tools::ToolExecutor::SubjectCheck check) {
    SessionRegistry* registry = registry_.load();
    if (registry == nullptr) return {false, "the server is not serving sessions yet"};
    const std::shared_ptr<Session> session = registry->Find(session_id);
    if (session == nullptr) return {false, "no such session: " + session_id};
    return session->CheckPermission(std::move(check));
}

}  // namespace minimal::agent
