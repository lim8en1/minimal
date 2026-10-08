#pragma once

// Lookup -> busy check -> confirm -> execute -> result.
//
// Deliberately does NOT decide WHERE it runs. The reader thread reads
// Command::ReadOnly() and either hands the invocation to a short-lived task or
// queues it for the worker; by the time Dispatch runs, that choice is made and
// this is on the right thread already. Putting the thread choice here would
// require this to know about the session's queue, which is the coupling the
// three-thread topology exists to avoid.

#include <string>

#include "minimal/commands/command.hpp"
#include "minimal/commands/command_context.hpp"
#include "minimal/commands/command_registry.hpp"

namespace minimal::commands {

// `busy` is "a turn is in flight". An LLMCommand needs the provider and the
// history to itself, so it is refused here. Production never reaches that:
// the reader (SessionServiceImpl::DispatchCommand) pauses the turn and queues
// the command for the boundary (Session::SubmitCompaction), so the worker
// always dispatches with busy == false.
//
// `project_registry`, when given, is consulted BEFORE `registry` -- see
// FindEffective. Defaulted so every caller with no project scope of its own
// (tests, DispatchSessionless's sibling calls) is unaffected.
CommandResult Dispatch(const CommandRegistry& registry, const std::string& name,
                       const CommandArgs& args, CommandContext& context, bool busy,
                       const CommandRegistry* project_registry = nullptr);

// The sessionless counterpart, for a CommandContext whose session ref is null
// (the RunSessionlessCommand RPC). Only a command that is BOTH ReadOnly() and
// Sessionless<> (RequiresSession() == false) may execute here; everything else
// -- LLM and prompt-shorthand kinds by nature, confirming commands because
// there is no session broker to round-trip through -- is refused with an
// error result rather than dropped, so a caller always gets an answer.
CommandResult DispatchSessionless(const CommandRegistry& registry, const std::string& name,
                                  const CommandArgs& args, CommandContext& context);

// The two refusal messages Dispatch() and the reader-thread pre-check
// (SessionServiceImpl::DispatchCommand, which must answer an unknown
// command before it is ever queued) both produce verbatim -- shared so the
// wording cannot drift between the two call graphs, which otherwise differ
// entirely (order of checks, extra read-only and no-registry cases on the
// reader side).
std::string UnknownCommandMessage(const std::string& name);
std::string BusyRefusalMessage(const std::string& name);
std::string SessionRequiredMessage(const std::string& name);
// A subagent's surface is read-only; the subagent guard (reader-side in
// SessionServiceImpl::DispatchCommand, defense-in-depth in
// Session::ExecuteCommand) refuses everything else verbatim with this.
std::string SubagentRefusalMessage(const std::string& name);

}  // namespace minimal::commands
