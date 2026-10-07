#pragma once

// One entry of the TUI's /-command catalog (command_catalog.hpp): what the
// prompt editor offers, and what a plugin listing commands reads through
// HostUi::CommandCatalog().

#include <string>

namespace minimal::client {

// One /-command the editor may offer. From ListCommands, pulled per tab
// scope by the host (CommandCatalogCache), plus client-intercepted entries.
struct CommandCatalogEntry {
    std::string name;
    std::string summary;
    bool read_only = false;
    bool submits_prompt = false;  // PromptShorthand: the one kind that submits a turn
    // Command::RequiresSession: excluded from /-autocomplete (and the /help
    // overlay) while the focused tab has no session -- a command that cannot
    // run without one is never offered as one.
    bool requires_session = true;
    // Command::SensitiveArgs: the line is kept out of the persisted history.
    bool sensitive_args = false;
};

}  // namespace minimal::client
