#include "mcp_command.hpp"

namespace minimal::mcp {

commands::CommandResult McpCommand::Execute(commands::CommandContext&,
                                            const commands::CommandArgs& args) {
    if (args.positional.empty()) return commands::CommandResult::Ok("mcp list requested");
    if (args.positional.size() != 2 ||
        (args.positional[1] != "enable" && args.positional[1] != "disable")) {
        return commands::CommandResult::Error("usage: /mcp <name> enable|disable");
    }
    const std::string& name = args.positional[0];
    const bool enabled = args.positional[1] == "enable";
    std::string error;
    if (!manager_.SetEnabled(name, enabled, error)) return commands::CommandResult::Error(error);
    return commands::CommandResult::Ok("mcp: " + name + " " + (enabled ? "enabled" : "disabled"));
}

}  // namespace minimal::mcp
