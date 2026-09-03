#include "minimal/commands/command_dispatcher.hpp"

namespace minimal::commands {

std::string UnknownCommandMessage(const std::string& name) {
    return "unknown command: /" + name + " (try /help)";
}

std::string BusyRefusalMessage(const std::string& name) {
    return "/" + name + " cannot run while a turn is in flight; cancel it first";
}

CommandResult Dispatch(const CommandRegistry& registry, const std::string& name,
                       const CommandArgs& args, CommandContext& context, bool busy) {
    Command* command = registry.Find(name);
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

    try {
        auto outcome = command->Execute(context, args);
        // The command owns where its result surfaces; the dispatcher only
        // carries it. Downgraded to the status line on error -- a View result
        // that failed has nothing to show, and errors belong on the notice
        // line (EXTENSIBILITY 4.4: "errors too, styled as errors").
        outcome.display = outcome.status == CommandResult::Status::kOk
                              ? command->DisplayFor(args)
                              : CommandDisplay::kStatus;
        if (outcome.display == CommandDisplay::kView) {
            outcome.view_hint = std::string(command->ViewHintFor(args));
        }
        return outcome;
    } catch (const std::exception& error) {
        // A throwing command must not take the worker down with it: the session
        // survives a failed turn, and it survives a failed command for the same
        // reason.
        context.logger().warn("session {}: /{} threw: {}", context.session().session_id(), name,
                              error.what());
        return CommandResult::Error("/" + name + " failed: " + error.what());
    }
}

}  // namespace minimal::commands
