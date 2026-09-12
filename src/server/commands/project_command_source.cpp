#include "minimal/commands/project_command_source.hpp"

#include "minimal/plugins/plugin_loader.hpp"

namespace minimal::commands {

const CommandRegistry& ProjectCommandSource::For(const std::filesystem::path& project_root) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = cache_.find(project_root); it != cache_.end()) return *it->second;

    // Built OUTSIDE any lock a plugin's own hook might reasonably want to
    // take is not possible here (mutex_ is already held) -- but unlike the
    // global registry's startup load, this runs at most once per DISTINCT
    // root over the process's life, not per session, so serializing it is
    // the same cost AgentPresetRegistry's cache pays for a concurrent miss.
    auto registry = std::make_unique<CommandRegistry>();
    if (!project_root.empty()) {
        loader_.RegisterProjectCommands(*registry, project_root);
    }
    auto [inserted, ok] = cache_.emplace(project_root, std::move(registry));
    (void)ok;   // emplace on a fresh key (just missed the find above) always succeeds
    return *inserted->second;
}

}  // namespace minimal::commands
