// tools-python -- the persistent python interpreter (python_exec), its
// polling/input surfaces (python_output, python_input), the interrupt surface
// (python_interrupt), /python, and a SessionObserver.
//
// All of these live together deliberately: the per-session interpreter registry
// lives INSIDE this plugin (core holds no python state), so /python is
// registered here, not in commands-core -- a command belongs to the plugin
// that owns the state it touches -- and OnSessionEnded must be implemented
// here or every interpreter that ever ran would stay resident for the life of
// the process.
//
// THE LIFETIME CONTRACT (the deliberate deviation from background bash):
// bash's background tasks are stateless processes, so a timer may kill them;
// the interpreter is SHARED SESSION STATE, so no SYNCHRONOUS wait ever
// destroys it -- timeouts demote a sync wait to async. Kills are explicit
// (python_interrupt(kill=true), clean=true, /python restart) or structural
// (session terminate, server exit). The one destructive timer is a request's
// own opt-in max_runtime: interrupt on expiry, hard-kill only if the request
// ignores SIGINT for the grace period. The reader thread polls
// ToolContext::session_lifetime and killpg's on expiry -- the same mechanism
// background bash uses, because SessionObserver::OnSessionEnded is
// fire-and-forget and cannot be a kill path.
//
// THE ENVIRONMENT CONTRACT: state is per-session (one interpreter per
// session), environment is per-project (ONE shared .venv, created on demand
// when the agent preset declares packages/requirements). The preset's declared
// package contract rides agents/*.md `tool_config.python` frontmatter -- the
// generic per-preset tool-config mechanism -- and is diffed against the
// interpreter's on every call: additive entries auto-install into the shared
// venv (immediately importable by the running interpreter), spec conflicts are
// reported loudly instead of silently re-versioning the shared environment.
//
// SECURITY MODEL: executed code runs with the full privileges of the local
// user; the permission gate is the boundary. Category Execute (Ask by
// default), disclosure names the resolved interpreter binary, and the same
// "model error, not malice" threat model as bash applies: against an
// adversarial model holding an interpreter, only a container helps.

#include "minimal/plugins/plugin_abi.h"

#include "python_command.hpp"
#include "python_settings.hpp"
#include "python_store.hpp"
#include "python_tools.hpp"

namespace {

// One store for the whole plugin: both tools, /python and the SessionObserver
// operate on the same map. Process-lifetime (plugins are never unloaded),
// matching the registries it feeds.
minimal::tools_python::InterpreterStore& Interpreters() {
    static minimal::tools_python::InterpreterStore store;
    return store;
}

// The settings are parsed once in minimal_plugin_register_tools (the loader
// runs register_tools first) and handed to each tool by value; the plugin has
// no process-global settings state.

// Releases this plugin's per-session state AND kills the interpreter. The
// observer is fire-and-forget, so the kill promptness belongs to the reader
// thread's session_lifetime poll; doing it here too is belt and braces (idempotent).
class PythonSessionObserver : public minimal::agent::SessionObserver {
public:
    void OnSessionEnded(const minimal::agent::SessionInfo& info) override {
        Interpreters().Erase(info.session_id, "session ended");
    }
};

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("tools-python")

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own `config` object from its plugins.json entry, verbatim;
    // core imposes no schema, this plugin owns the shape of its own slice.
    // Per-preset DECLARED package contracts arrive separately, per invocation,
    // on ToolContext::tool_config (the `tool_config.python` frontmatter slice).
    const auto settings = minimal::tools_python::ParsePythonSettings(context.config);
    registry.Register(
        minimal::tools_python::MakePythonExecTool(settings, Interpreters()));
    registry.Register(
        minimal::tools_python::MakePythonOutputTool(settings, Interpreters()));
    registry.Register(
        minimal::tools_python::MakePythonInputTool(settings, Interpreters()));
    registry.Register(
        minimal::tools_python::MakePythonInterruptTool(settings, Interpreters()));
}

void minimal_plugin_register_commands(
    minimal::commands::CommandRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_unique<minimal::tools_python::PythonCommand>(&Interpreters()));
}

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_shared<PythonSessionObserver>());
}

}  // extern "C"
