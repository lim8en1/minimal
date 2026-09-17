#include "minimal/commands/command_registry.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::commands {

void CommandRegistry::Register(std::unique_ptr<Command> command) {
    if (!command) return;
    const std::string original(command->Name());

    // Enforced at the door, because nothing at the dispatch site can: a
    // read-only command runs on the bounded RPC pool beside live turns, while
    // RequiresConfirmation blocks on a round-trip to a human. The combination
    // deadlocks one pool slot per invocation (and the pool is bounded), so the
    // registration is refused rather than tolerated.
    if (command->ReadOnly() && command->RequiresConfirmation()) {
        common::DefaultLogger().warn(
            "command '{}' is read-only AND requires confirmation; refused -- a confirming "
            "read-only command would deadlock the bounded command pool",
            original);
        return;
    }

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

std::vector<CommandRegistry::Entry> CommandRegistry::All() const {
    std::vector<Entry> out;
    out.reserve(commands_.size());
    for (const auto& [name, command] : commands_) out.push_back(Entry{name, command.get()});
    // Unordered_map iteration order is unspecified, so /help would otherwise
    // list differently run to run. Sorted by SURFACE name, not Name(): the
    // surface key is what the user can actually type.
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.surface < b.surface; });
    return out;
}

void CommandRegistry::RemoveByAttribution(const std::string& plugin_name) {
    // See the header: pre-collision-check, pre-alias rollback only.
    //
    // First-wins (Register: "first registration owns the slot"), so the
    // WINNER of a surface name is whichever attribution for it appears FIRST
    // in registration order -- computed here, from the full list, before any
    // entries are removed. A losing attribution for the same surface is kept
    // only for collision diagnostics and must never affect whether the
    // winner's slot gets erased: checking "does ANY surviving attribution
    // still name this surface" (the previous approach) let a leftover
    // loser's record block erasure of a winning plugin's command during that
    // winning plugin's own rollback, and conversely erasing on a LOSER's own
    // removal would wrongly drop a slot the loser never owned.
    std::unordered_map<std::string, std::string> winner_plugin;  // surface -> plugin_name
    for (const auto& attr : attributions_) {
        winner_plugin.try_emplace(attr.surface_name, attr.plugin_name);
    }

    std::vector<Attribution> kept;
    kept.reserve(attributions_.size());
    std::vector<std::string> surfaces_owned_by_removed;
    for (auto& attr : attributions_) {
        if (attr.plugin_name == plugin_name) {
            if (winner_plugin[attr.surface_name] == plugin_name) {
                surfaces_owned_by_removed.push_back(attr.surface_name);
            }
        } else {
            kept.push_back(std::move(attr));
        }
    }
    attributions_ = std::move(kept);
    for (const auto& surface : surfaces_owned_by_removed) {
        commands_.erase(surface);
    }
}

}  // namespace minimal::commands
