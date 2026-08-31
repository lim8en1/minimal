#pragma once

#include <string_view>

#include "background_registry.hpp"
#include "minimal/commands/command.hpp"

namespace minimal::tools_bash {

// /tasks -- the operator's window into background bash. Lives in this plugin,
// not commands-core, because the task registry is plugin-owned state (the same
// rule that put /restart-shell here): a command belongs to the plugin that owns
// the state it touches.
//
// ReadOnly: listing touches nothing, so it may answer while a turn is in
// flight -- which is exactly when the operator wants to check on a task.
// Deliberately the ONLY kill/list surface besides the model's own `kill -- -PID`:
// the state is small, and a bespoke view or RPC for it would be surface built
// ahead of its consumer.
class TasksCommand : public minimal::commands::HarnessCommand {
public:
    explicit TasksCommand(BackgroundTaskRegistry* registry) : registry_(registry) {}

    std::string_view Name() const override { return "tasks"; }
    std::string_view Summary() const override {
        return "List this session's background bash tasks";
    }
    std::string_view Usage() const override { return "/tasks"; }
    bool ReadOnly() const override { return true; }

    minimal::commands::CommandResult Execute(minimal::commands::CommandContext& context,
                                             const minimal::commands::CommandArgs&) override;

private:
    // Borrowed, process-lifetime (a static inside this .so). Null is legal the
    // same way it is for restart_shell's service lookup: answer rather than
    // crash, with a message that says what is missing.
    BackgroundTaskRegistry* registry_;
};

}  // namespace minimal::tools_bash
