#include "minimal/agent/hook_observer_bridge.hpp"

namespace minimal::hooks {
namespace {

// session_created and session_ended describe the session the same way.
nlohmann::json SessionPayload(const agent::SessionInfo& info) {
    return nlohmann::json{
        {"session_id", info.session_id},
        {"parent_session_id", info.parent_session_id},
        {"name", info.name},
        {"agent_preset", info.agent_preset},
        {"project_root", info.project_root.string()},
    };
}

}  // namespace

void HookObserverBridge::OnSessionCreated(const agent::SessionInfo& info) {
    dispatcher_.EmitAsync(kSessionCreated, SessionPayload(info));
}

void HookObserverBridge::OnSessionEnded(const agent::SessionInfo& info) {
    dispatcher_.EmitAsync(kSessionEnded, SessionPayload(info));
}

void HookObserverBridge::OnTurnCompleted(const agent::SessionInfo& info,
                                         const agent::TurnSummary& summary) {
    dispatcher_.EmitAsync(kStop, nlohmann::json{
                                     {"session_id", info.session_id},
                                     {"project_root", info.project_root.string()},
                                     {"stop_reason", StopReasonName(summary.stop_reason)},
                                     {"duration_ms", summary.duration.count()},
                                     {"tool_names_invoked", summary.tool_names_invoked},
                                 });
}

void HookObserverBridge::OnToolExecuted(const agent::SessionInfo& info,
                                        const agent::ToolInvocation& invocation) {
    dispatcher_.EmitAsync(kPostToolUse, nlohmann::json{
                                            {"session_id", info.session_id},
                                            {"project_root", info.project_root.string()},
                                            {"tool_use_id", invocation.tool_use_id},
                                            {"tool_name", invocation.tool_name},
                                            // The registered filter fields: without
                                            // them a `category`/`permission_key`
                                            // filter on this point never matched.
                                            {"permission_key", invocation.permission_key},
                                            {"category", invocation.category},
                                            {"summary", invocation.summary},
                                            {"is_error", invocation.is_error},
                                            {"duration_ms", invocation.duration.count()},
                                        });
}

}  // namespace minimal::hooks
