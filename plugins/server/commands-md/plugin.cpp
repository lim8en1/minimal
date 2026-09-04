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
std::vector<std::filesystem::path> CommandDirs() {
    std::vector<std::filesystem::path> dirs;
    dirs.push_back(minimal::common::ConfigDir() / "commands");
    const std::filesystem::path cwd = std::filesystem::current_path();
    if (!cwd.empty()) {
        const minimal::config::ProjectDirs project(cwd);
        for (const auto& dir : project.dirs()) {
            dirs.push_back(dir / "commands");
            dirs.push_back(dir / ".opencode" / "commands");
        }
    }
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
    for (auto& command : minimal::commands_md::LoadCommands(CommandDirs())) {
        registry.Register(std::move(command));
    }
}

}  // extern "C"
