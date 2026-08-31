#pragma once

// What a command is allowed to reach.
//
// SessionControl is an INTERFACE rather than `Session&` for two reasons. It
// keeps commands/ from depending on agent/Session (which would be a cycle, since
// the session builds and runs these), and it means a command can be tested
// against a fake without a worker thread, a provider and a transcript store.
//
// WHAT IS DELIBERATELY ABSENT is as much of the design as what is present:
//
//   - No bash session state. Bash is a plugin and owns its own per-session
//     cwd/env; one .so cannot reach into another's. That is exactly why
//     /restart-shell lives in tools-bash rather than here, and why
//     register_tools and register_commands are independent ABI symbols.
//   - No SessionRegistry. /new and /attach would need it, and they are deferred
//     to the TUI (track E) where a client can act on the result. Handing every
//     command the power to create sessions before anything can attach to one
//     would be surface built ahead of its consumer.

#include <optional>
#include <string>

#include "minimal/agent/agent_loop.hpp"
#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/tools/tool_executor.hpp"

namespace minimal::commands {

// The session, as much of it as a command may touch. Implemented by
// agent::Session.
//
// EVERY METHOD HERE IS WORKER-THREAD ONLY unless its own comment says otherwise,
// and that is what makes the mutating ones safe without a lock: a command that
// changes session state is not ReadOnly(), so it is queued and run by the worker
// between turns -- the same thread that reads this state when building the next
// request. The exceptions (preset_snapshot) return copies and are safe anywhere.
class SessionControl {
public:
    virtual ~SessionControl() = default;

    virtual const std::string& session_id() const = 0;
    virtual agent::AgentLoop& loop() = 0;
    virtual tools::ToolExecutor& executor() = 0;

    // The preset this session is actually running: the registry's copy with any
    // model override already applied.
    //
    // WORKER-THREAD ONLY -- it returns a reference into state ApplyPreset
    // rewrites, so only a mutating command (which the worker runs between turns)
    // may hold it. A read-only command executes on the RPC layer's pool, not the
    // worker, and must use preset_snapshot() below.
    virtual const agent::AgentPreset& preset() const = 0;

    // A copy taken under the session lock; safe from any thread. This is what a
    // read-only command reads.
    virtual agent::AgentPreset preset_snapshot() const = 0;

    // Replaces it, in the session AND in the loop -- there are two copies, and a
    // command updating only one is a bug that shows up a turn later.
    virtual void ApplyPreset(agent::AgentPreset preset) = 0;

    // The user's explicit model choice, if any. Owned by the session rather than
    // living in the preset, because it has to SURVIVE /agent re-resolving that
    // preset: a preset supplies a default, a user selection is a decision.
    virtual const std::optional<common::ModelRef>& model_override() const = 0;
    virtual void set_model_override(std::optional<common::ModelRef> ref) = 0;
};

class CommandContext {
public:
    CommandContext(SessionControl& session, const agent::AgentPresetRegistry& presets,
                   ConfirmationBroker& confirmations, common::Logger& logger)
        : session_(session), presets_(presets), confirmations_(confirmations), logger_(logger) {}

    SessionControl& session() { return session_; }
    const agent::AgentPresetRegistry& presets() const { return presets_; }
    const config::ProvidersConfig& providers() const { return presets_.providers(); }
    ConfirmationBroker& confirmations() { return confirmations_; }
    common::Logger& logger() { return logger_; }

private:
    SessionControl& session_;
    const agent::AgentPresetRegistry& presets_;
    ConfirmationBroker& confirmations_;
    common::Logger& logger_;
};

}  // namespace minimal::commands
