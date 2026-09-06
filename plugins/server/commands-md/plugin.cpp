// commands-md -- markdown-defined /-commands, as one plugin.
//
// Discovery, per docs/OPENCODE_COMPAT.md Phase 4: the global config dir's
// commands/ plus the project's commands/ and .opencode/commands/ (opencode's
// own layout, so existing command files work zero-copy). The nearest directory
// wins a stem collision -- the same nearest-first rule project-local agents and
// skills follow. The process cwd is the project anchor, exactly as it is for
// session creation the client performs; the plugin cannot know a session's
// stored root at registration, and commands are registered once, at startup.
//
// Template logic is deliberately ALL here, plugin-local (consumer-first rule):
// the argument split, file desugaring, and shell handling promote to a shared
// header only when a second real consumer appears.

#include <filesystem>

#include "commands_md.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/config/project_dirs.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace {

// Project-local command directories, nearest first, bounded the same way the
// agents/skills walk is: the cwd, then ancestors to the git root. Reused here
// through config::ProjectDirs so the three discovery surfaces cannot disagree
// about what "the project" is.
//
// PROJECT DIRECTORIES FIRST, the global config dir LAST: LoadCommands keeps
// the first spec it sees per stem, so walk order IS shadow order, and
// "nearest directory wins; project shadows global" (the documented rule, and
// what the collision-warning message itself claims) requires project entries
// to be seen before the global one. Each entry is tagged trusted/untrusted
// per minimal::commands_md::CommandDir -- the global dir is the operator's own
// config; the project dirs are working-tree-supplied content a command file
// there did not ask the operator to trust (see MdCommandSpec::trusted).
std::vector<minimal::commands_md::CommandDir> CommandDirs() {
    std::vector<minimal::commands_md::CommandDir> dirs;
    // LIMITATION, stated precisely: the anchor is the SERVER process's cwd,
    // not any session's project root. PluginRegistrationContext carries no
    // project root (commands register once, at startup, before any session
    // exists -- and the server serves many project roots), so project-local
    // command files are discovered only under the directory the server was
    // launched from. A session whose project_root differs sees only the
    // global (trusted) commands from this plugin.
    const std::filesystem::path cwd = std::filesystem::current_path();
    if (!cwd.empty()) {
        const minimal::config::ProjectDirs project(cwd);
        for (const auto& dir : project.dirs()) {
            dirs.push_back({dir / "commands", /*trusted=*/false});
            dirs.push_back({dir / ".opencode" / "commands", /*trusted=*/false});
        }
    }
    dirs.push_back({minimal::common::ConfigDir() / "commands", /*trusted=*/true});
    return dirs;
}

}  // namespace

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "commands-md"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    for (auto& command : minimal::commands_md::LoadCommands(
             CommandDirs(), [&registry](const minimal::commands_md::MdCommandSpec& spec) {
                 // An untrusted (project-local) file whose command name is
                 // already taken is SKIPPED, not registered: a duplicate
                 // surface name is a fatal startup collision, so a cloned
                 // repo's commands/help.md would otherwise refuse to let the
                 // server start. The earlier registration wins.
                 return minimal::commands_md::AcceptNonColliding(registry, spec);
             })) {
        registry.Register(std::move(command));
    }
}

}  // extern "C"
