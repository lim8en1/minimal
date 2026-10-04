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
// cached per root and REBUILT when the plugins' fingerprint for that root
// changes (PluginLoader::ProjectCommandsFingerprint -- commands-md reports its
// files' mtimes), so editing a project command file needs no restart.

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "minimal/commands/command_registry.hpp"
#include "minimal/config/project_trust.hpp"

namespace minimal::commands {

// What ProjectCommandSource needs from whoever owns the project-command
// contributors (the plugin loader implements it): keeps this module free of
// any dependency on the plugin host above it.
class ProjectCommandProvider {
public:
    virtual ~ProjectCommandProvider() = default;
    // Fills `registry` with the commands project_root contributes.
    virtual void RegisterProjectCommands(CommandRegistry& registry,
                                         const std::filesystem::path& project_root) const = 0;
    // Changes whenever what RegisterProjectCommands would produce changes;
    // empty when nothing contributes.
    virtual std::string ProjectCommandsFingerprint(
        const std::filesystem::path& project_root) const = 0;
};

class ProjectCommandSource {
public:
    // `trust` (null = no gate) decides whether a root's project-local commands
    // load at all; an untrusted root gets a permanently empty registry. The
    // decision is made per call, ahead of the cache, so a trust change takes
    // effect on the next lookup. Must outlive this source.
    explicit ProjectCommandSource(const ProjectCommandProvider& loader,
                                  const config::ProjectTrust* trust = nullptr)
        : loader_(loader), trust_(trust) {}

    // Never null. An empty project_root (a session with none) resolves to a
    // permanently-empty registry -- there is nothing to discover for it, and
    // a stable empty instance is simpler than every caller special-casing
    // "no root" separately. The path is used exactly as given; canonicalizing
    // two spellings of the same root onto one cache entry, if ever needed, is
    // the caller's job (SessionRegistry already canonicalizes what it hands
    // AgentPresetRegistry).
    //
    // Shared ownership, because a rebuild can replace a root's registry while
    // a command from the old one is still executing (a markdown command's
    // generators run for seconds): hold the pointer for as long as you use
    // anything found in it, and the old registry lives exactly that long.
    std::shared_ptr<const CommandRegistry> For(const std::filesystem::path& project_root) const;

private:
    struct Entry {
        std::shared_ptr<const CommandRegistry> registry;
        std::string fingerprint;
    };
    const ProjectCommandProvider& loader_;
    const config::ProjectTrust* trust_;
    std::shared_ptr<const CommandRegistry> empty_ = std::make_shared<const CommandRegistry>();
    mutable std::mutex mutex_;
    // GUARDED by mutex_: built lazily, on whichever thread asks first (a
    // gRPC handler resolving ListCommands, a session worker dispatching a
    // command) -- there is no single startup thread to build it on, unlike
    // the global registry.
    mutable std::map<std::filesystem::path, Entry> cache_;
};

}  // namespace minimal::commands
