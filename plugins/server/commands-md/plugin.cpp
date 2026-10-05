// commands-md -- markdown-defined /-commands, as one plugin.
//
// Discovery: the global config dir's
// commands/ plus the project's commands/ and .opencode/commands/ (opencode's
// own layout, so existing command files work zero-copy). The nearest directory
// wins a stem collision -- the same nearest-first rule project-local agents and
// skills follow.
//
// TWO SEPARATE HOOKS, because they run at two different times against two
// different registries:
//   * minimal_plugin_register_commands -- the GLOBAL config dir only, once at
//     startup, into the process-wide CommandRegistry. There is no project
//     root yet at startup (commands register before any session exists), so
//     this hook cannot see one -- which is exactly why project-local
//     discovery used to be anchored on the SERVER PROCESS's own cwd instead,
//     a workaround that broke entirely under the client's normal auto-spawn
//     path (server_spawn.cpp chdir()s to "/" before exec).
//   * minimal_plugin_register_project_commands -- the PROJECT's own dirs,
//     called lazily per project root (ProjectCommandSource), into a FRESH
//     registry scoped to that root alone -- and called AGAIN when
//     minimal_plugin_project_commands_fingerprint (the dirs' and files'
//     mtimes) changes, so editing a project command needs no restart. Never shares a registry with the
//     global hook: a project command may shadow a global PROMPT SHORTHAND of
//     the same name for sessions rooted there, never a harness/LLM command
//     (commands::ProjectMayShadow, applied at lookup).
//
// Template logic is deliberately ALL here, plugin-local (consumer-first rule):
// the argument split, file desugaring, and shell handling promote to a shared
// header only when a second real consumer appears.

#include <filesystem>
#include <string>

#include "commands_md.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/config/project_dirs.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace {

// The one global, trusted discovery dir -- the operator's own config, not
// working-tree-supplied content. Registered once, at startup.
std::vector<minimal::commands_md::CommandDir> GlobalCommandDirs() {
    return {{minimal::common::ConfigDir() / "commands", /*trusted=*/true}};
}

// Project-local command directories for ONE root, nearest first, bounded the
// same way the agents/skills walk is: `project_root` and its ancestors up to
// the git root. Reused here through config::ProjectDirs so the three
// discovery surfaces cannot disagree about what "the project" is. Both
// entries are untrusted (see MdCommandSpec::trusted): working-tree-supplied
// content a command file there did not ask the operator to trust.
std::vector<minimal::commands_md::CommandDir> ProjectCommandDirs(
    const std::filesystem::path& project_root) {
    std::vector<minimal::commands_md::CommandDir> dirs;
    if (project_root.empty()) return dirs;
    const minimal::config::ProjectDirs project(project_root);
    for (const auto& dir : project.dirs()) {
        dirs.push_back({dir / "commands", /*trusted=*/false});
        dirs.push_back({dir / ".opencode" / "commands", /*trusted=*/false});
    }
    return dirs;
}

void LoadInto(minimal::commands::CommandRegistry& registry,
             const std::vector<minimal::commands_md::CommandDir>& dirs) {
    for (auto& command : minimal::commands_md::LoadCommands(dirs)) {
        registry.Register(std::move(command));
    }
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("commands-md")

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    LoadInto(registry, GlobalCommandDirs());
}

void minimal_plugin_register_project_commands(
    minimal::commands::CommandRegistry& registry, const std::filesystem::path& project_root,
    const minimal::plugins::PluginRegistrationContext&) {
    LoadInto(registry, ProjectCommandDirs(project_root));
}

void minimal_plugin_describe_project_content(
    const std::filesystem::path& project_root,
    std::vector<minimal::config::ProjectContentCount>& out) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& dir : ProjectCommandDirs(project_root)) {
        for (std::filesystem::directory_iterator it(dir.path, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->path().extension() == ".md") ++count;
        }
        ec.clear();
    }
    out.push_back({"command", "commands", count});
}

void minimal_plugin_project_commands_fingerprint(const std::filesystem::path& project_root,
                                                 std::string& out) {
    try {
        out = minimal::commands_md::FingerprintCommandDirs(ProjectCommandDirs(project_root));
    } catch (...) {
        out.clear();   // the ABI forbids throwing; an unreadable dir reads as "no commands"
    }
}

}  // extern "C"
