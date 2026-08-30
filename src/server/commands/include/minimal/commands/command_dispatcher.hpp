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
// history to itself, so it is refused rather than queued behind the turn --
// queueing would make /compact silently run minutes later against a context that
// had moved on.
CommandResult Dispatch(const CommandRegistry& registry, const std::string& name,
                       const CommandArgs& args, CommandContext& context, bool busy);

}  // namespace minimal::commands
