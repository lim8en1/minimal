#pragma once

// The project-scoped half of the /-command catalog.
//
// minimal_plugin_register_commands (commands-md's global hook) runs ONCE at
// startup, before any session exists -- so it has no project root to anchor
// project-local discovery on (<project_root>/commands/,
// .opencode/commands/). The old workaround anchored on the SERVER PROCESS's
// own cwd instead, which the client's auto-spawn path chdir()s to "/" before
// exec -- so under the normal spawn, project-local commands were discovered,
// period. See docs/COMMANDS_HOWTO.md and plugins/server/commands-md/README.md.
//
// This is the actual anchor: one CommandRegistry PER project root, built
// lazily the first time a session rooted there asks for it (For()), by
// asking every loaded plugin's OPTIONAL minimal_plugin_register_project_commands
// hook to contribute (PluginLoader::RegisterProjectCommands). The result is
// cached for the process's life -- config is static once loaded, the same
// rule AgentPresetRegistry's identical per-(name,root) cache already follows
// for agent presets.

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include "minimal/commands/command_registry.hpp"

namespace minimal::plugins {
class PluginLoader;
}

namespace minimal::commands {

class ProjectCommandSource {
public:
    explicit ProjectCommandSource(const plugins::PluginLoader& loader) : loader_(loader) {}

    // Never null. An empty project_root (a session with none) resolves to a
    // permanently-empty registry -- there is nothing to discover for it, and
    // a stable empty instance is simpler than every caller special-casing
    // "no root" separately. The path is used exactly as given; canonicalizing
    // two spellings of the same root onto one cache entry, if ever needed, is
    // the caller's job (SessionRegistry already canonicalizes what it hands
    // AgentPresetRegistry).
    const CommandRegistry& For(const std::filesystem::path& project_root) const;

private:
    const plugins::PluginLoader& loader_;
    mutable std::mutex mutex_;
    // GUARDED by mutex_: built lazily, on whichever thread asks first (a
    // gRPC handler resolving ListCommands, a session worker dispatching a
    // command) -- there is no single startup thread to build it on, unlike
    // the global registry.
    mutable std::map<std::filesystem::path, std::unique_ptr<CommandRegistry>> cache_;
};

}  // namespace minimal::commands
