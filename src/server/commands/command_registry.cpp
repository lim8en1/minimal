#include "minimal/commands/command_registry.hpp"

#include <algorithm>
#include <utility>

namespace minimal::commands {

void CommandRegistry::Register(std::unique_ptr<Command> command) {
    if (!command) return;
    const std::string original(command->Name());

    // The alias is keyed by stable identity -- "plugin.original_name" -- which is
    // why it can be written before anyone knows whether a collision exists.
    std::string surface = original;
    if (!attribution_.empty()) {
        const auto it = aliases_.find(attribution_ + "." + original);
        if (it != aliases_.end()) surface = it->second;
    }

    // Recorded even when the surface name is a duplicate: the collision check
    // needs BOTH sides to name them, and a registry that dropped the loser would
    // leave it with only one.
    attributions_.push_back(Attribution{attribution_, original, surface});
    commands_.insert_or_assign(std::move(surface), std::move(command));
}

Command* CommandRegistry::Find(std::string_view name) const {
    const auto it = commands_.find(std::string(name));
    return it == commands_.end() ? nullptr : it->second.get();
}

std::vector<const Command*> CommandRegistry::All() const {
    std::vector<const Command*> out;
    out.reserve(commands_.size());
    for (const auto& [name, command] : commands_) out.push_back(command.get());
    // Unordered_map iteration order is unspecified, so /help would otherwise
    // list differently run to run.
    std::sort(out.begin(), out.end(),
              [](const Command* a, const Command* b) { return a->Name() < b->Name(); });
    return out;
}

}  // namespace minimal::commands
