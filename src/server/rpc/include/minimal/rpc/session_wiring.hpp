#pragma once

// Wires a session's transport-facing pieces: the EventTranslator transport,
// the (possibly advisor-composed) permission checker, and the confirmation
// and ask brokers. main.cpp hands this function to SessionRegistry as its
// SessionWiring step, so agent/ never names rpc/ -- it only ever sees
// agent::SessionTransport, tools::PermissionChecker,
// commands::ConfirmationBroker and tools::AskBroker.
//
// Composition call sites (SessionServiceImpl::ApplyAdvisor, SubagentManager's
// advisor inheritance) only SELECT an advisor, via
// SessionParams::selected_advisor/advisor_timeout; this is what BUILDS
// everything, once per session, after unattended_prompt_timeout resolved.

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
