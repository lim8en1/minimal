// tools-windbg -- the WinDbg tool pair (windbg, windbg_interrupt), as a
// CLIENT of a dbgd server: the debugger engine (DbgEng via pybag, with all
// of dbg-cli's isolation and remoting machinery) lives in the dbgd process
// on the debugger host; this plugin maps the dbg-cli wire protocol onto the
// harness's tool + non-blocking-task machinery.
//
// State ownership, following the same rule as tools-bash: the per-session
// dbgd session map AND the background-job registry live INSIDE this plugin
// (core holds no windbg state), so OnSessionEnded must be implemented here
// or both the map and the remote sessions would leak for the process
// lifetime. Remote teardown has two idempotent paths: the explicit Erase
// below, and the per-session lifetime watcher (sidecar::WatchLifetime) that
// fires when ToolContext::session_lifetime expires first -- there is no
// synchronous kill hook at teardown, so the poll is the backstop, exactly as
// for background bash's process group.

#include "minimal/plugins/plugin_abi.h"
#include "minimal/sidecar/task_registry.hpp"

#include "dbgd_client.hpp"
#include "windbg_session.hpp"
#include "windbg_settings.hpp"
#include "windbg_tools.hpp"

namespace {

// Process-lifetime statics, seeded on first (registration) call with the
// parsed config -- the same pattern as tools-bash's BashStore(inherit_env).
minimal::tools_windbg::WindbgSettings& Settings(
    const minimal::tools_windbg::WindbgSettings* seed = nullptr) {
    static minimal::tools_windbg::WindbgSettings settings(
        seed != nullptr ? *seed : minimal::tools_windbg::WindbgSettings{});
    return settings;
}

minimal::tools_windbg::DbgdClient& Client(
    const minimal::tools_windbg::WindbgSettings* seed = nullptr) {
    static minimal::tools_windbg::DbgdClient client(
        seed != nullptr ? *seed : minimal::tools_windbg::WindbgSettings{});
    return client;
}

minimal::tools_windbg::WindbgSessionStore& Store(
    minimal::tools_windbg::DbgdClient* client = nullptr,
    const std::string* on_exit = nullptr) {
    static minimal::tools_windbg::WindbgSessionStore store(*client, *on_exit);
    return store;
}

minimal::sidecar::TaskRegistry& Jobs() {
    static minimal::sidecar::TaskRegistry registry;
    return registry;
}

class WindbgSessionObserver : public minimal::agent::SessionObserver {
public:
    void OnSessionEnded(const minimal::agent::SessionInfo& info) override {
        Store().Erase(info.session_id, "session ended");
        Jobs().EraseSession(info.session_id);
    }
};

}  // namespace

extern "C" {

const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "tools-windbg"; }
const char* minimal_plugin_version() { return minimal::common::Version(); }

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own `config` object (base_url, bearer_token, backend,
    // default_target, ...): parse once, seed the process-lifetime statics.
    const auto settings = minimal::tools_windbg::ParseWindbgSettings(context.config);
    Settings(&settings);
    Client(&settings);
    Store(&Client(), &Settings().on_session_end);

    registry.Register(minimal::tools_windbg::MakeWindbgTool(Settings(), Client(), Store(),
                                                            Jobs()));
    registry.Register(
        minimal::tools_windbg::MakeWindbgInterruptTool(Settings(), Client(), Store()));
}

void minimal_plugin_register_session_observer(
    minimal::agent::SessionObserverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_shared<WindbgSessionObserver>());
}

}  // extern "C"
