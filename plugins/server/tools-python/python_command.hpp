#pragma once

#include <string_view>

#include "minimal/commands/command.hpp"

#include "python_store.hpp"

namespace minimal::tools_python {

// /python -- the operator's window into this plugin's interpreters, living in
// the plugin that owns the state (the /restart-shell and /tasks rule).
// ReadOnly: listing touches nothing; `restart` is the operator's kill that
// does not depend on the model asking nicely.
class PythonCommand : public minimal::commands::HarnessCommand {
public:
    explicit PythonCommand(InterpreterStore* store) : store_(store) {}

    std::string_view Name() const override { return "python"; }
    std::string_view Summary() const override {
        return "List this session's python interpreters (or restart them)";
    }
    std::string_view Usage() const override { return "/python [restart]"; }
    bool ReadOnly() const override { return false; }  // restart kills a process

    minimal::commands::CommandResult Execute(minimal::commands::CommandContext& context,
                                             const minimal::commands::CommandArgs& args) override;

private:
    // Borrowed, process-lifetime (a static inside this .so). Null answers
    // rather than crashes, like /tasks.
    InterpreterStore* store_;
};

}  // namespace minimal::tools_python
