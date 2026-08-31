// tools-bash -- the bash tool, plus (when their core subsystems land) the
// /restart-shell command and a SessionObserver.
//
// All three live together deliberately. The per-session cwd/env map lives INSIDE
// this plugin (core holds no bash state), so:
//   - /restart-shell must be registered here, not in commands-core: one .so cannot
//     reach into another's state.
//   - OnSessionEnded must be implemented here, or that map grows for the lifetime
//     of the process.
// This is the canonical example of plugin-owned per-session state needing a
// lifecycle signal.
//
// Split from tools-fs because it has a distinct security profile and is the one
// tool with a plausible alternate implementation (container / remote executor)
// someone would actually swap. Replacing it needs no new mechanism: disable this
// plugin and ship one registering `bash`.
//
// SECURITY MODEL: commands run through /bin/sh -c, and the permission gate is the
// boundary -- bash is category Execute (Ask by default) and the operator approves
// the command. There is no allowlist and no attempt at one: keyed on name it dies
// to `cp /bin/bash /tmp/git`, keyed on path or hash it dies to the interpreters
// you must allow anyway (git aliases, make, xargs, env). The threat model is model
// ERROR, not model malice; against an adversarial model holding a shell only a
// container helps, which is what the swap-out path above is for.
//
// Consent scoping (see ARCHITECTURE.md) makes Ask livable: an invocation discloses
// the binaries it names, by resolved path, and the operator may grant them for the
// session. That reduces prompts; it does not constrain what a command can do.

#include "minimal/plugins/plugin_abi.h"

#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"

namespace {

// One store for the whole plugin: the tool, /restart-shell and the
// SessionObserver all operate on this same map -- three callers, one owner,
// which is what "a plugin owns the commands that touch its own state" buys.
// Process-lifetime (plugins are never unloaded), matching the registries it feeds.
minimal::tools_bash::BashSessionStateStore& BashStore() {
    static minimal::tools_bash::BashSessionStateStore store;
    return store;
}

// Releases this plugin's per-session state. Core cannot do it: the map lives in
// this .so and nothing outside it can reach the store. Without this hook the
// cwd/env of every session that ever ran would stay resident for the life of
// the process.
// THE PLUGIN-OWNS-ITS-COMMANDS RULE, made concrete. /restart-shell lives here
// rather than in commands-core because the per-session cwd/env it resets lives
// inside THIS .so -- another plugin has no way to reach it. That is why
// minimal_plugin_register_tools and ..._register_commands are independent
// optional symbols instead of one "kind" of plugin: this one exports both.
class RestartShellCommand : public minimal::commands::HarnessCommand {
public:
    std::string_view Name() const override { return "restart-shell"; }
    std::string_view Summary() const override {
        return "Forget this session's shell cwd and environment";
    }
    std::string_view Usage() const override { return "/restart-shell"; }

    minimal::commands::CommandResult Execute(minimal::commands::CommandContext& context,
                                             const minimal::commands::CommandArgs&) override {
        // Exactly what `restart: true` on a bash call does, and what
        // OnSessionEnded does at teardown -- one map, one owner, three callers.
        BashStore().Erase(context.session().session_id());
        return minimal::commands::CommandResult::Ok(
            "shell reset; the next command starts in the project root with a fresh environment");
    }
};

class BashSessionObserver : public minimal::agent::SessionObserver {
public:
    void OnSessionEnded(const minimal::agent::SessionInfo& info) override {
        BashStore().Erase(info.session_id);
    }
};

}  // namespace

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-bash"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own `config` object from its plugins.json entry, verbatim;
    // core imposes no schema, this plugin owns the shape of its own slice.
    const auto settings = minimal::tools_bash::ParseBashSettings(context.config);
    registry.Register(minimal::tools_bash::MakeBashTool(settings, BashStore()));
}

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_unique<RestartShellCommand>());
}

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_shared<BashSessionObserver>());
}

}  // extern "C"
