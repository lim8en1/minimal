#include "tasks_command.hpp"

#include <chrono>
#include <sstream>

#include "minimal/commands/command_context.hpp"

namespace minimal::tools_bash {

minimal::commands::CommandResult TasksCommand::Execute(
    minimal::commands::CommandContext& context, const minimal::commands::CommandArgs&) {
    if (registry_ == nullptr) {
        return minimal::commands::CommandResult::Error(
            "the background task registry is unavailable");
    }
    const auto tasks = registry_->List(context.session().session_id());
    if (tasks.empty()) {
        return minimal::commands::CommandResult::Ok("no background tasks in this session");
    }

    std::ostringstream out;
    for (const auto& task : tasks) {
        out << task.id << "  ";
        switch (task.state) {
            case tools::BackgroundTaskEvent::State::kStarted:
                out << "running  ";
                break;
            case tools::BackgroundTaskEvent::State::kCompleted:
                out << "done     ";
                break;
            case tools::BackgroundTaskEvent::State::kFailed:
                out << "failed   ";
                break;
            case tools::BackgroundTaskEvent::State::kTimedOut:
                out << "timed out";
                break;
            case tools::BackgroundTaskEvent::State::kKilled:
                out << "killed   ";
                break;
        }
        const auto seconds = task.elapsed_ms / 1000;
        out << "  " << (seconds < 60 ? std::to_string(seconds) + "s"
                                     : std::to_string(seconds / 60) + "m" +
                                           std::to_string(seconds % 60) + "s");
        out << "  pid " << task.pid << "  " << task.command << '\n';
    }
    return minimal::commands::CommandResult::Ok(out.str());
}

}  // namespace minimal::tools_bash
