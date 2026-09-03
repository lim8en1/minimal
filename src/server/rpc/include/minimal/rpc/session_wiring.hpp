#pragma once

// Shared advisor-composition recipe for a session's permission checker.
// Duplicated identically at two sites before this existed --
// rpc::SessionServiceImpl's top-level-session path (permissions.json's
// configured advisor) and agent::SubagentManager's advisor-inheritance path
// (a parent's advisor, opted into per spawn) -- structurally the same four
// steps: make the channels a wire checker needs, mint a cancel holder, wrap
// the wire checker in AdvisedPermissionChecker, stash the holder in params
// for Session to install its own predicate into.
//
// Deliberately narrow for now: it does not yet cover the OTHER two pieces
// every session needs regardless of advisor (the default WirePermissionChecker
// when no advisor applies, and the always-built WireConfirmationBroker) --
// those still come from agent::Session's own constructor fallback. Widening
// this into the session's one unconditional wiring recipe is later Phase 4
// work, once SessionChannels' transport half (EventBroadcaster,
// PendingRequests) splits out from its domain half (InjectionQueue,
// background_tasks_live) -- see docs/SERVER_CORE_FIXES.md Phase 4.

#include <chrono>
#include <memory>

#include "minimal/agent/session.hpp"
#include "minimal/tools/permission_advisor.hpp"

namespace minimal::rpc {

// No-op (leaves `params` untouched) when advisor is null: the caller's params
// then carry no channels/checker, and Session's own ctor fallback builds the
// wire default -- the human, as always.
void ApplyPermissionAdvisor(agent::SessionParams& params,
                             std::shared_ptr<tools::PermissionAdvisor> advisor,
                             std::chrono::milliseconds advisor_timeout);

}  // namespace minimal::rpc
