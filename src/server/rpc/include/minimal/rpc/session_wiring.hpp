#pragma once

// The one place agent:: code (SessionRegistry's creation funnel) reaches into
// rpc:: to wire a session's transport-facing pieces: the EventTranslator
// behind SessionParams::event_sink, the (possibly advisor-composed)
// permission checker, and the always-built WireConfirmationBroker. Nothing
// downstream of this file ever names those concrete types again -- agent/
// only ever sees SessionEventSink, tools::PermissionChecker,
// commands::ConfirmationBroker, and an opaque transport_handle.
//
// ONE PHASE, by construction: a composition call site
// (SessionServiceImpl::ApplyAdvisor, SubagentManager's advisor-inheritance
// path) only ever SELECTS an advisor -- it sets the plain
// SessionParams::selected_advisor/advisor_timeout fields and makes no rpc::
// call at all. FinishSessionWiring, called once per session from
// SessionRegistry::CreateResolved after SessionParams::unattended_prompt_timeout
// has resolved, is what actually BUILDS everything: the composed checker (if
// selected_advisor is set -- its underlying wire checker always waits
// forever, so it never needed the timeout, but building it early bought
// nothing once selection and composition were split), the default
// WirePermissionChecker otherwise, and always the WireConfirmationBroker.

#include <chrono>
#include <memory>

#include "minimal/agent/session.hpp"

namespace minimal::rpc {

// Called once per session, after params.unattended_prompt_timeout is
// resolved, for EVERY session (top-level or subagent, advised or not).
// Composes params.selected_advisor into params.permission_checker when set;
// otherwise builds the default WirePermissionChecker. Always (re)builds the
// WireConfirmationBroker, since nothing upstream of this ever supplies one.
void FinishSessionWiring(agent::SessionParams& params);

}  // namespace minimal::rpc
