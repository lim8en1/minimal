#include "minimal/commands/project_command_source.hpp"

namespace minimal::commands {

std::shared_ptr<const CommandRegistry> ProjectCommandSource::For(
    const std::filesystem::path& project_root) const {
    if (trust_ != nullptr && !trust_->Allows(project_root)) return empty_;
    // Outside the lock: stat-level work per plugin, and a lookup for one
    // root must not wait on another root's fingerprint. An empty root has
    // nothing to discover and so nothing to fingerprint.
    const std::string fingerprint =
        project_root.empty() ? std::string{} : loader_.ProjectCommandsFingerprint(project_root);

    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = cache_.find(project_root);
        it != cache_.end() && it->second.fingerprint == fingerprint) {
        return it->second.registry;
    }

    // A miss or a stale entry. Built under mutex_ (a plugin hook cannot take
    // any lock it might want here without deadlocking, same as before): this
    // runs once per distinct root plus once per edit, not per lookup, so
    // serializing it is the cost AgentPresetRegistry's cache pays for a
    // concurrent miss. The replaced registry stays alive for whoever still
    // holds it.
    auto registry = std::make_shared<CommandRegistry>();
    if (!project_root.empty()) {
        loader_.RegisterProjectCommands(*registry, project_root);
    }
    Entry& entry = cache_[project_root];
    entry.registry = std::move(registry);
    entry.fingerprint = fingerprint;
    return entry.registry;
}

}  // namespace minimal::commands
