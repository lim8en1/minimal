#pragma once

// Bridges SessionObserver notifications onto hook points (track O).
//
// The observe-only hook points need NO dispatch machinery of their own: the
// SessionObserver registry already provides the exact properties they require
// -- one ordered thread, fire-and-forget, throw-contained, owned payloads,
// drop-counting. This bridge converts four observer callbacks into four
// EmitAsync calls and inherits all of it. The veto-capable points
// (pre_tool_use, user_prompt_submit) do NOT come through here: they are sync
// emissions from their own seams (ToolExecutor, the session worker).
//
// Registered in main BEFORE plugin observers, so a hook action a user mapped
// onto a lifecycle point runs before plugin code reacting to the same event.

#include <nlohmann/json.hpp>

#include "minimal/agent/session_observer.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"

namespace minimal::hooks {

class HookObserverBridge : public agent::SessionObserver {
public:
    explicit HookObserverBridge(const HookDispatcher& dispatcher) : dispatcher_(dispatcher) {}

    void OnSessionCreated(const agent::SessionInfo& info) override;
    void OnTurnCompleted(const agent::SessionInfo& info,
                         const agent::TurnSummary& summary) override;
    void OnToolExecuted(const agent::SessionInfo& info,
                        const agent::ToolInvocation& invocation) override;
    void OnSessionEnded(const agent::SessionInfo& info) override;

private:
    const HookDispatcher& dispatcher_;
};

}  // namespace minimal::hooks
