#include "minimal/commands/command_dispatcher.hpp"

namespace minimal::commands {

CommandResult Dispatch(const CommandRegistry& registry, const std::string& name,
                       const CommandArgs& args, CommandContext& context, bool busy) {
    Command* command = registry.Find(name);
    if (command == nullptr) {
        // Answered, never dropped: a client waiting on a CommandResult would
        // otherwise hang on a request the server quietly discarded.
        return CommandResult::Error("unknown command: /" + name + " (try /help)");
    }

    if (busy && !command->ReadOnly()) {
        return CommandResult::Error("/" + name +
                                    " cannot run while a turn is in flight; cancel it first");
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
        return command->Execute(context, args);
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
