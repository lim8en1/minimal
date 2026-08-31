#include "restart_shell.hpp"

#include "bash_state_service.hpp"
#include "minimal/commands/command_context.hpp"

namespace minimal::tools_bash {

RestartShellCommand::RestartShellCommand(const minimal::plugins::ServiceRegistry* services)
    : services_(services) {}

std::string_view RestartShellCommand::Name() const { return "restart-shell"; }

std::string_view RestartShellCommand::Summary() const {
    return "Forget this session's shell cwd and environment";
}

std::string_view RestartShellCommand::Usage() const { return "/restart-shell"; }

minimal::commands::CommandResult RestartShellCommand::Execute(
    minimal::commands::CommandContext& context, const minimal::commands::CommandArgs&) {
    auto* service =
        services_ == nullptr
            ? nullptr
            : static_cast<BashSessionStateService*>(services_->Find(kBashStateServiceName));
    if (service == nullptr) {
        return minimal::commands::CommandResult::Error(
            "shell state service is unavailable: tools-bash did not provide '" +
            std::string(kBashStateServiceName) +
            "' (is tools-bash disabled, or built before the service registry?)");
    }

    // Exactly what `restart: true` on a bash call does, and what
    // OnSessionEnded does at teardown -- one map, one owner, three callers.
    service->Erase(context.session().session_id());
    return minimal::commands::CommandResult::Ok(
        "shell reset; the next command starts in the project root with a fresh environment");
}

}  // namespace minimal::tools_bash
