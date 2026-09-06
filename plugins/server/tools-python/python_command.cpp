#include "python_command.hpp"

#include <sstream>

#include "minimal/commands/command_context.hpp"

namespace minimal::tools_python {

minimal::commands::CommandResult PythonCommand::Execute(
    minimal::commands::CommandContext& context, const minimal::commands::CommandArgs& args) {
    if (store_ == nullptr) {
        return minimal::commands::CommandResult::Error("the interpreter store is unavailable");
    }
    if (!args.positional.empty() && args.positional[0] == "restart") {
        store_->Erase(context.session().session_id(), "restarted by /python restart");
        return minimal::commands::CommandResult::Ok(
            "interpreter killed; the next python_exec starts a fresh one");
    }
    if (!args.positional.empty()) {
        return minimal::commands::CommandResult::Error("usage: /python [restart]");
    }

    const auto rows = store_->List(context.session().session_id());
    if (rows.empty()) {
        return minimal::commands::CommandResult::Ok(
            "no python interpreter in this session (python_exec starts one)");
    }
    std::ostringstream out;
    for (const auto& row : rows) {
        out << "pid " << row.pid << (row.alive ? "  alive" : "  dead") << "  " << row.python
            << (row.version.empty() ? "" : " (" + row.version + ")") << '\n';
        out << "  environment: " << (row.venved ? "project venv" : "system python")
            << ", declared entries: " << row.declared_count << '\n';
        if (!row.pending.empty()) {
            out << "  pending:";
            for (const auto& id : row.pending) out << ' ' << id;
            out << '\n';
        }
    }
    return minimal::commands::CommandResult::Ok(out.str());
}

}  // namespace minimal::tools_python
