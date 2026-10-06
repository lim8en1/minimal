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
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::milliseconds(task.elapsed_ms));
        const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(elapsed);
        out << "  "
            << (minutes.count() == 0
                    ? std::to_string(elapsed.count()) + "s"
                    : std::to_string(minutes.count()) + "m" +
                          std::to_string((elapsed - minutes).count()) + "s");
        out << "  pid " << task.pid << "  " << task.command << '\n';
    }
    return minimal::commands::CommandResult::Ok(out.str());
}

}  // namespace minimal::tools_bash
