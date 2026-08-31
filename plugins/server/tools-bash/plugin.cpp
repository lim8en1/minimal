// tools-bash -- the bash tool, the /restart-shell command, the BashSessionState
// service, and a SessionObserver.
//
// All four live together deliberately. The per-session cwd/env map lives INSIDE
// this plugin (core holds no bash state), so:
//   - /restart-shell must be registered here, not in commands-core: a command
//     should belong to the plugin that owns the state it touches. Other plugins
//     now reach that state through the BashSessionStateService provided below
//     -- the sanctioned channel (docs/EXTENSIBILITY.md §2.2), which is what
//     lets the command itself resolve the store through ServiceRegistry::Find
//     instead of a plugin-internal static.
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
#include "restart_shell.hpp"

namespace {

// One store for the whole plugin: the tool, /restart-shell and the
// SessionObserver all operate on this same map -- three callers, one owner,
// which is what "a plugin owns the commands that touch its own state" buys.
// Process-lifetime (plugins are never unloaded), matching the registries it feeds.
minimal::tools_bash::BashSessionStateStore& BashStore() {
    static minimal::tools_bash::BashSessionStateStore store;
    return store;
}

// This plugin's hook point handle, minted at registration and captured here
// (the same process-lifetime rule as BashStore). register_hook_points runs
// BEFORE register_tools in the loader precisely so the handle exists when the
// tool is constructed.
minimal::hooks::HookPoint& BeforeCommandPoint() {
    static minimal::hooks::HookPoint point;
    return point;
}

// Releases this plugin's per-session state. Core cannot do it: the map lives in
// this .so and nothing outside it can reach the store. Without this hook the
// cwd/env of every session that ever ran would stay resident for the life of
// the process.
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
    registry.Register(
        minimal::tools_bash::MakeBashTool(settings, BashStore(), BeforeCommandPoint()));
}

// The worked example for plugin-declared hook points (docs/EXTENSIBILITY.md):
// a veto-capable event fired right before this plugin spawns a shell command.
// The point is declared here; what a Block MEANS is also declared here -- this
// plugin refuses the invocation -- which is exactly the division of labor the
// hook mechanism specifies: core owns dispatch and fail-open semantics, the
// plugin owns its events and their consequences.
void minimal_plugin_register_hook_points(
    minimal::hooks::HookDispatcher& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    BeforeCommandPoint() = registry.RegisterPoint(
        {minimal::tools_bash::kBeforeCommandPoint, /*veto_capable=*/true,
         /*filterable=*/{"program"}});
}

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The consumer side of the service channel: the command captures the
    // registry pointer here and resolves the store lazily at execute time.
    registry.Register(
        std::make_unique<minimal::tools_bash::RestartShellCommand>(context.services));
}

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_shared<BashSessionObserver>());
}

// Provides the per-session shell state under the shipped abstract contract
// (bash_state_service.hpp). Non-owning pointer is safe for the process
// lifetime: plugin handles are never dlclosed, and the store is a process-
// lifetime static inside this .so.
void minimal_plugin_register_services(
    minimal::plugins::ServiceRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Provide(minimal::tools_bash::kBashStateServiceName, &BashStore());
}

}  // extern "C"
