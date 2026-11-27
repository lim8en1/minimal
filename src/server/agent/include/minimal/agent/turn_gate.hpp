#pragma once

// Turn gates: plugins that may REJECT a turn's prompt before anything of it is
// persisted or sent to a provider -- the in-process counterpart of the
// core.user_prompt_submit hook (a shell action), for a plugin whose verdict
// depends on the session's effective preset.
//
// Reachable from the plugin ABI: minimal_plugin_register_turn_gates
// (plugin_abi.h). The node plugin -- a preset's input contract, checked
// against tool_config.node.input_schema -- is the first consumer; nothing
// here knows about it.
//
// WHERE: on the session worker at turn start, AFTER the per-turn preset
// override is applied (so `--agent X` on a resumed session is gated by X),
// for top-level turns with a prompt only -- the same scope as the hook veto
// (a subagent's task is its parent's formulation; a seedless wake carries no
// prompt). A rejection persists nothing, emits an error carrying the reason,
// and completes the turn with StopReason::Rejected.
//
// CONTRACT for implementers:
//   - Check() runs synchronously on the session worker; keep it local and
//     cheap (no network, no provider calls).
//   - nullopt passes. A string rejects; it is shown to the client verbatim.
//   - Throwing FAILS CLOSED: the turn is rejected naming the plugin. A gate
//     guards a contract, and a broken guard must not wave input through.
//   - Gates run in registration (plugins.json load) order; the first
//     rejection wins and later gates are not asked.

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::agent {

struct TurnGateRequest {
    std::string session_id;
    // The preset this turn runs under (a per-turn override included).
    const AgentPreset& preset;
    // The prompt as the client sent it, before @-mention expansion.
    const std::string& prompt;
};

class TurnGate {
public:
    virtual ~TurnGate() = default;
    virtual std::optional<std::string> Check(const TurnGateRequest& request) = 0;
};

// Additive: every plugin may register any number of gates.
class TurnGateRegistry {
public:
    explicit TurnGateRegistry(common::Logger& logger);

    TurnGateRegistry(const TurnGateRegistry&) = delete;
    TurnGateRegistry& operator=(const TurnGateRegistry&) = delete;

    // Attribution window, the loader's shape for every registry.
    void SetAttribution(std::string plugin_name);
    void ClearAttribution();

    // Startup-only in production (plugin load). A gate registered outside an
    // attribution window is attributed to "core".
    void Register(std::shared_ptr<TurnGate> gate);

    // Registration-time rollback for a plugin whose hook threw.
    void RemoveByAttribution(const std::string& plugin_name);
    std::size_t CountOwnedBy(const std::string& plugin_name) const;

    // The first rejection, or nullopt when every gate passes.
    std::optional<std::string> Check(const TurnGateRequest& request) const;

private:
    struct Entry {
        std::string plugin;
        std::shared_ptr<TurnGate> gate;
    };

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
    std::string attribution_;
    common::Logger& logger_;
};

}  // namespace minimal::agent
