#pragma once

// The one place agent:: code (SessionServiceImpl's advisor composition,
// SubagentManager's advisor-inheritance composition, SessionRegistry's
// creation funnel) reaches into rpc:: to wire a session's transport-facing
// pieces: the EventTranslator behind SessionParams::event_sink, the default
// WirePermissionChecker, and the always-built WireConfirmationBroker. Nothing
// downstream of this file ever names those concrete types again -- agent/
// only ever sees SessionEventSink, tools::PermissionChecker,
// commands::ConfirmationBroker, and an opaque transport_handle.
//
// TWO PHASES, because of a real timing constraint: the advisor decision
// (which advisor, if any) is made by the caller BEFORE
// SessionParams::unattended_prompt_timeout has been resolved --
// SessionRegistry::CreateResolved's funnel (session_registry.cpp) is what
// resolves it, and that runs strictly after both composition call sites
// (SessionServiceImpl::ApplyAdvisor, SubagentManager's spawn path). An
// advised session's underlying wire checker never uses that timeout anyway
// (it always waits forever -- the human fallback has no budget), so
// ApplyPermissionAdvisor can safely run early; but the CONFIRMATION broker
// (built for every session, advised or not) and the DEFAULT permission
// checker (built only when no advisor applies) both need the resolved value,
// so they wait for FinishSessionWiring, called from CreateResolved once the
// funnel has run.
//
//   early  (session_service.cpp / subagent_manager.cpp): ApplyPermissionAdvisor
//   late   (session_registry.cpp, after unattended_prompt_timeout resolves):
//          FinishSessionWiring

#include <chrono>
#include <memory>

#include "minimal/agent/session.hpp"
#include "minimal/tools/permission_advisor.hpp"

namespace minimal::rpc {

// No-op (leaves `params` untouched) when advisor is null: the caller's params
// then carry no transport/checker, and FinishSessionWiring builds the wire
// default later.
void ApplyPermissionAdvisor(agent::SessionParams& params,
                             std::shared_ptr<tools::PermissionAdvisor> advisor,
                             std::chrono::milliseconds advisor_timeout);

// Called once per session, after params.unattended_prompt_timeout is
// resolved, for EVERY session (top-level or subagent, advised or not).
// Builds whatever ApplyPermissionAdvisor left unbuilt -- the transport and
// its EventTranslator if none exists yet, the default WirePermissionChecker
// if params.permission_checker is still null -- and always (re)builds the
// WireConfirmationBroker, since nothing upstream of this ever supplies one.
void FinishSessionWiring(agent::SessionParams& params);

}  // namespace minimal::rpc
