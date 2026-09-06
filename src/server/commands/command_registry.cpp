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
    // First registration owns the slot, matching ToolRegistry -- insert_or_assign
    // here made the two registries disagree about who held the slot while the
    // fatal check was pending, for no reason: which object wins is moot when the
    // collision is fatal, so the registries should not disagree at all.
    commands_.emplace(std::move(surface), std::move(command));
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

void CommandRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // See the header: pre-collision-check, pre-alias rollback only.
    std::vector<Attribution> kept;
    kept.reserve(attributions_.size());
    std::vector<std::string> surfaces;
    for (auto& attr : attributions_) {
        if (attr.plugin_name == plugin_name) {
            surfaces.push_back(std::move(attr.surface_name));
        } else {
            kept.push_back(std::move(attr));
        }
    }
    attributions_ = std::move(kept);
    for (const auto& surface : surfaces) {
        // First-wins: the slot holds the FIRST registrant, so erase only when
        // no surviving attribution still claims the same surface name.
        const bool claimed = std::any_of(
            attributions_.begin(), attributions_.end(),
            [&](const Attribution& attr) { return attr.surface_name == surface; });
        if (!claimed) commands_.erase(surface);
    }
}

}  // namespace minimal::commands
