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
//     cwd/env; the state is reachable only through the sanctioned channel --
//     ServiceRegistry::Find against the provider's interface header -- which is
//     why /restart-shell lives in tools-bash: a command belongs to the plugin
//     that owns the state it touches. Services are the way TO ASK for state;
//     this context still hands out no blanket reach into other plugins.
//   - No SessionRegistry. /new and /attach would need it, and they are deferred
//     to the TUI (track E) where a client can act on the result. Handing every
//     command the power to create sessions before anything can attach to one
//     would be surface built ahead of its consumer.

#include <cassert>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/commands/confirmation_broker.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/config/providers_config.hpp"
#include "minimal/tools/tool_executor.hpp"

namespace minimal::agent {
class AgentLoop;  // only a reference crosses this interface; users include agent_loop.hpp
}

namespace minimal::commands {

class CommandRegistry;

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

    // The session's project root -- the confinement boundary, and the anchor
    // for project-local discovery (agents/, skills/, commands/). Never empty
    // for a real session: CreateSession requires it. Empty is legal for test
    // fakes and means "no project scope" everywhere it is consumed.
    virtual std::filesystem::path project_root() const = 0;

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

    // The session's explicit subagent-model choice (/subagent-model): what
    // model its spawned children run when the spawn call itself names none.
    // Same session-owned-decision reasoning as model_override above.
    virtual const std::optional<common::ModelRef>& subagent_model_override() const {
        static const std::optional<common::ModelRef> kNone;
        return kNone;
    }
    virtual void set_subagent_model_override(std::optional<common::ModelRef> ref) {
        (void)ref;  // a control surface that never grew the feature ignores it
    }

    // Renames this session (/rename): persists the name and notifies every
    // attached client via a partial SessionMetadataEvent, so tab labels
    // refresh. Returns false when the name is empty or the write failed; the
    // caller surfaces that as a command error. Unlike the titler's rename
    // seam, a USER rename is unconditional -- it clobbers any previous name
    // and stamps name_source='explicit', which is what makes a later titler
    // pass skip the session.
    //
    // Safe from any thread (read-only commands run on the RPC pool): the
    // in-memory half synchronizes on the session mutex and the database
    // serializes its own writers.
    virtual bool RenameSession(const std::string& name) {
        (void)name;
        return false;  // a control surface that never grew the feature refuses
    }

    // A predicate a long-running command polls to notice the user cancelling
    // (CancelTurn) or the session terminating while it runs -- the same
    // signal that ends a turn, scoped to "since this probe was made", so a
    // cancel that ended an EARLIER turn does not pre-cancel the command. Make
    // it at the start of Execute; use it only within that call. The default
    // never fires (a control surface that never grew the feature).
    virtual std::function<bool()> CancelProbe() const {
        return [] { return false; };
    }

    // Submits a user turn on this session -- the ONE sanctioned path by which a
    // PromptShorthandCommand's content reaches the model (docs/EXTENSIBILITY.md
    // 4.4): as the submitted turn itself, appended to history and answered like
    // any other turn, never as a command result. Same queue and contract as a
    // client-submitted turn.
    virtual bool SubmitTurn(std::string text) = 0;

    // SubmitTurn with PER-TURN model/agent overrides: the turn runs on the
    // given model ("<provider>/<model>", already split by the caller) and/or
    // agent preset, resolved at pop; the session's sticky choices are
    // untouched. Default ignores the overrides and submits a plain turn, so a
    // control surface that never grew the feature degrades rather than
    // diverges; Session overrides it for real.
    virtual bool SubmitTurnWithOverrides(std::string text,
                                         const std::optional<common::ModelRef>& model,
                                         const std::string& agent_preset) {
        (void)model;
        (void)agent_preset;
        return SubmitTurn(std::move(text));
    }
};

class CommandContext {
public:
    // `session` is NULLABLE: a sessionless invocation (DispatchSessionless)
    // passes nullptr, and only a Sessionless<> command may be dispatched that
    // way -- the dispatcher refuses everything else before Execute, so a
    // session-bound command can never observe the null. Commands reach the
    // session through session() (asserts non-null); a sessionless-capable one
    // branches on has_session() first.
    // `project_commands` is NULLABLE: the project-scoped half of the catalog
    // this invocation's session sees (ProjectCommandSource), null when there
    // is no session or no project scope. A command that lists or explains
    // other commands (/help) reads it through EffectiveCatalog/FindEffective,
    // so it sees exactly what dispatch runs.
    CommandContext(SessionControl* session, const agent::AgentPresetRegistry& presets,
                   ConfirmationBroker& confirmations, common::Logger& logger,
                   const CommandRegistry* project_commands = nullptr)
        : session_(session),
          presets_(presets),
          confirmations_(confirmations),
          logger_(logger),
          project_commands_(project_commands) {}

    // The session, as much of a command may touch. Implemented by agent::Session.
    // ONLY legal when has_session() -- a Sessionless<> command must branch first.
    SessionControl& session() {
        assert(session_ != nullptr && "session() on a sessionless CommandContext");
        return *session_;
    }
    SessionControl* session_if() const { return session_; }
    bool has_session() const { return session_ != nullptr; }

    const agent::AgentPresetRegistry& presets() const { return presets_; }
    const config::ProvidersConfig& providers() const { return presets_.providers(); }
    ConfirmationBroker& confirmations() { return confirmations_; }
    common::Logger& logger() { return logger_; }
    const CommandRegistry* project_commands() const { return project_commands_; }

private:
    SessionControl* session_;
    const agent::AgentPresetRegistry& presets_;
    ConfirmationBroker& confirmations_;
    common::Logger& logger_;
    const CommandRegistry* project_commands_;
};

}  // namespace minimal::commands
