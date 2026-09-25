#pragma once

#include "minimal/commands/command.hpp"
#include "minimal/mcp/mcp_client_manager.hpp"

namespace minimal::mcp {

// Bare /mcp opens the client's MCP picker (display=View, view_hint "mcp" --
// the mcp-view client plugin's view, which reads the server list through this
// plugin's endpoint). "/mcp <name> enable|disable" toggles LIVE via
// McpClientManager::SetEnabled -- no server restart, unlike editing mcps.json.
//
// Sessionless AND ReadOnly: an MCP server's enabled state is process-global,
// not session state, so it never touches the conversation path -- the exact
// meaning ReadOnly() carries here, not "mutates nothing".
class McpCommand final : public commands::Sessionless<commands::HarnessCommand> {
public:
    // Borrowed; the plugin owns it until shutdown, after every session ended.
    explicit McpCommand(McpClientManager& manager) : manager_(manager) {}

    std::string_view Name() const override { return "mcp"; }
    std::string_view Summary() const override {
        return "View MCP servers, or enable/disable one live";
    }
    std::string_view Usage() const override { return "/mcp [name enable|disable]"; }
    bool ReadOnly() const override { return true; }

    // The bare form is a picker, the argument form a direct action.
    commands::CommandDisplay Display() const override { return commands::CommandDisplay::kView; }
    std::string_view ViewHint() const override { return "mcp"; }
    commands::CommandDisplay DisplayFor(const commands::CommandArgs& args) const override {
        return args.positional.empty() ? commands::CommandDisplay::kView
                                       : commands::CommandDisplay::kStatus;
    }
    std::string_view ViewHintFor(const commands::CommandArgs& args) const override {
        return args.positional.empty() ? ViewHint() : std::string_view{};
    }
    bool ReadOnlyFor(const commands::CommandArgs&) const override { return true; }

    commands::CommandResult Execute(commands::CommandContext& context,
                                    const commands::CommandArgs& args) override;

private:
    McpClientManager& manager_;
};

}  // namespace minimal::mcp
