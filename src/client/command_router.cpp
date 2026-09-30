#include "minimal/client/command_router.hpp"

#include <utility>

namespace minimal::client {

void CommandRouter::Add(ClientCommand command) {
    std::string name = command.name;
    commands_.insert_or_assign(std::move(name), std::move(command));
}

std::vector<CommandCatalogEntry> CommandRouter::CatalogEntries() const {
    std::vector<CommandCatalogEntry> entries;
    for (const auto& [name, command] : commands_) {
        if (command.summary.empty()) continue;
        entries.push_back({name, command.summary, command.read_only,
                           /*submits_prompt=*/false, command.requires_session});
    }
    return entries;
}

bool CommandRouter::Dispatch(const std::string& line) const {
    const CommandInvocationText parsed = SplitCommand(line);
    if (parsed.name.empty()) return false;
    const auto it = commands_.find(parsed.name);
    if (it == commands_.end() || !it->second.run) return false;
    return it->second.run(parsed);
}

}  // namespace minimal::client
