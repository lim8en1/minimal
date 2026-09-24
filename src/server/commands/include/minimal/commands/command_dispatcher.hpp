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

// Resolves `name` against `project` first (if given and non-null), falling
// back to `global` -- the ONE precedence rule every dispatch path must agree
// on, project-scoped commands (ProjectCommandSource) included: a project
// command must never read as "unknown" to the reader thread's pre-check
// (SessionServiceImpl::DispatchCommand) while still running fine through
// Dispatch below, or vice versa. A project command may shadow a global
// PROMPT SHORTHAND of the same name (matching how a project's own
// agents/<name>.md shadows a global preset by name), but never a harness or
// LLM command: project commands are working-tree content, and a repo-shipped
// commands/connect.md taking over /connect would ship the API key the user
// typed to the model as a turn. See ProjectMayShadow.
//
// True when a project command may take the surface name `global_command`
// holds: nothing global holds it, or the holder is itself a prompt
// shorthand. The ONE rule both dispatch (FindEffective) and the catalog
// (ListCommands) apply, so what autocomplete shows is what runs.
bool ProjectMayShadow(const Command* global_command);

Command* FindEffective(const CommandRegistry& global, const CommandRegistry* project,
                       const std::string& name);

// Every command FindEffective can reach, sorted by surface name: the project
// entries that may shadow (ProjectMayShadow), then every global entry no
// project entry shadowed. What ListCommands advertises and /help lists, so
// the catalog never shows a command dispatch would not run.
std::vector<CommandRegistry::Entry> EffectiveCatalog(const CommandRegistry& global,
                                                     const CommandRegistry* project);

// `busy` is "a turn is in flight". An LLMCommand needs the provider and the
// history to itself, so it is refused rather than queued behind the turn --
// queueing would make /compact silently run minutes later against a context that
// had moved on.
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
// (SessionServiceImpl::DispatchCommand, which must answer an unknown/busy
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
