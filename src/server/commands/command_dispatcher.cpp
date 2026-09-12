#include "minimal/commands/command_dispatcher.hpp"

namespace minimal::commands {

Command* FindEffective(const CommandRegistry& global, const CommandRegistry* project,
                       const std::string& name) {
    if (project != nullptr) {
        if (Command* found = project->Find(name)) return found;
    }
    return global.Find(name);
}

std::string UnknownCommandMessage(const std::string& name) {
    return "unknown command: /" + name + " (try /help)";
}

std::string BusyRefusalMessage(const std::string& name) {
    return "/" + name + " cannot run while a turn is in flight; cancel it first";
}

std::string SessionRequiredMessage(const std::string& name) {
    return "/" + name + " needs a session; send a message to start one first";
}

namespace {

// The shared execute-and-shape tail of both dispatch paths: run the command,
// map the outcome's display/view_hint for THIS invocation, and contain a
// throw. `session_label` feeds the failure log line -- a sessionless context
// has no id to print.
CommandResult ExecuteAndShape(Command& command, const std::string& name,
                              const CommandArgs& args, CommandContext& context,
                              const std::string& session_label) {
    try {
        auto outcome = command.Execute(context, args);
        // The command owns where its result surfaces; the dispatcher only
        // carries it. Downgraded to the status line on error -- a View result
        // that failed has nothing to show, and errors belong on the notice
        // line (EXTENSIBILITY 4.4: "errors too, styled as errors").
        outcome.display = outcome.status == CommandResult::Status::kOk
                              ? command.DisplayFor(args)
                              : CommandDisplay::kStatus;
        if (outcome.display == CommandDisplay::kView) {
            outcome.view_hint = std::string(command.ViewHintFor(args));
        }
        // Derived from the TYPE, not trusted from the command (see
        // CommandResult::submitted_turn): a PromptShorthandCommand's own
        // contract is "Execute enqueues the turn and answers with an empty
        // kOk" (command.hpp), so an Ok result from one always means a turn
        // was just submitted and is still running. dynamic_cast is the same
        // pattern ListCommands and the sessionless guard above already use to
        // derive a command's kind, rather than trusting a per-command flag a
        // future PromptShorthandCommand author could forget to set.
        outcome.submitted_turn = outcome.status == CommandResult::Status::kOk &&
                                 dynamic_cast<const PromptShorthandCommand*>(&command) != nullptr;
        return outcome;
    } catch (const std::exception& error) {
        // A throwing command must not take the worker down with it: the session
        // survives a failed turn, and it survives a failed command for the same
        // reason.
        context.logger().warn("session {}: /{} threw: {}", session_label, name, error.what());
        return CommandResult::Error("/" + name + " failed: " + error.what());
    } catch (...) {
        // Plugin commands can throw anything. Non-standard exceptions would
        // otherwise escape into the session worker and terminate the process.
        context.logger().warn("session {}: /{} threw a non-standard exception", session_label,
                              name);
        return CommandResult::Error("/" + name + " failed: non-standard exception");
    }
}

}  // namespace

CommandResult Dispatch(const CommandRegistry& registry, const std::string& name,
                       const CommandArgs& args, CommandContext& context, bool busy,
                       const CommandRegistry* project_registry) {
    Command* command = FindEffective(registry, project_registry, name);
    if (command == nullptr) {
        // Answered, never dropped: a client waiting on a CommandResult would
        // otherwise hang on a request the server quietly discarded.
        return CommandResult::Error(UnknownCommandMessage(name));
    }

    // An LLMCommand specifically, not any non-read-only command. A mutating
    // HarnessCommand (`/agent`, `/model`) is MEANT to queue and run at the next
    // turn boundary -- that is what the session's shared work queue is for, and
    // what keeps it from landing between a tool_use and its tool_result. Only an
    // LLMCommand needs the provider and the history to itself, and only it is
    // pointless once the context it was aimed at has moved on. `!ReadOnly()` was
    // true of both, so this refused commands the queue exists to serve.
    if (busy && dynamic_cast<const LLMCommand*>(command) != nullptr) {
        return CommandResult::Error(BusyRefusalMessage(name));
    }

    if (command->RequiresConfirmation()) {
        ConfirmationRequest request;
        request.kind = "command_confirmation";
        request.title = "/" + std::string(command->Name());
        request.detail = std::string(command->Summary());
        request.options = {"Yes", "No"};

        const auto answer = context.confirmations().RequestConfirmation(request);
        if (!answer.answered()) {
            // No answer at all -- cancelled, or the session is going away. Not a
            // choice the user made, so it is reported rather than treated as a
            // decline they would have to undo.
            return CommandResult::Error("/" + std::string(command->Name()) + " was not confirmed");
        }
        if (answer.chosen_option != "Yes") {
            return CommandResult::Ok("/" + std::string(command->Name()) + " cancelled");
        }
    }

    // The label is computed before the call, not inside its catch: a sessioned
    // Dispatch context always has an id to print, and ExecuteAndShape already
    // contains Execute's own throw.
    return ExecuteAndShape(*command, name, args, context, context.session().session_id());
}

CommandResult DispatchSessionless(const CommandRegistry& registry, const std::string& name,
                                  const CommandArgs& args, CommandContext& context) {
    Command* command = registry.Find(name);
    if (command == nullptr) {
        // Same answered-never-dropped rule as Dispatch: the unary caller maps
        // an error result to its notice line either way.
        return CommandResult::Error(UnknownCommandMessage(name));
    }

    // THE SESSIONLESS GUARD. RequiresSession() is the taxonomy answer; the
    // LLM/prompt-shorthand and confirmation checks are defense-in-depth (a
    // Sessionless<LLMCommand> would be a nonsense registration, but the types
    // -- not conventions -- are what is asked here, the same rule the busy
    // check runs on). There is no session worker and no broker behind a
    // sessionless context, so anything that queues, blocks on a human, or
    // touches the conversation path has nothing to stand on.
    if (command->RequiresSession()) {
        return CommandResult::Error(SessionRequiredMessage(name));
    }
    // ReadOnly() too: a sessionless invocation runs INLINE on the gRPC
    // handler thread (RunSessionlessCommand), never on the bounded command
    // pool -- the same reasoning that keeps a mutating command off the
    // reader thread on the streamed path. A Sessionless<> command that is
    // not also ReadOnly() would block that thread.
    if (!command->ReadOnly()) {
        return CommandResult::Error(SessionRequiredMessage(name));
    }
    if (dynamic_cast<const LLMCommand*>(command) != nullptr ||
        dynamic_cast<const PromptShorthandCommand*>(command) != nullptr) {
        return CommandResult::Error(SessionRequiredMessage(name));
    }
    if (command->RequiresConfirmation()) {
        return CommandResult::Error("/" + name + " cannot ask for confirmation without a session");
    }

    return ExecuteAndShape(*command, name, args, context, "sessionless");
}

}  // namespace minimal::commands
