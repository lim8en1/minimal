#pragma once

// Owns every live session, and runs the lifetime rules: the initial-attach
// guard, the detached-idle reaper, and the process idle exit.
//
// THE GUARD IS A SAFETY NET, NOT A DESIGNED FLOW. Attaching to a session --
// including a subagent child announced by SubagentStartedEvent -- is a client
// protocol obligation. The rule is not "unattached => terminate", because a
// subagent emits events before its tab attaches; it is "never-yet-attached =>
// terminate after a short grace period", which only fires against a client that
// failed its obligation. Once a session has been attached, stream drop is what
// DETACHES it (S5): the session outlives its clients and is collected by the
// detached-idle reaper only after it has been fully quiescent for
// sessions.detached_idle_ttl_seconds. See ARCHITECTURE.md "Session lifetime".

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/context_contributor.hpp"
#include "minimal/agent/provider_resolver.hpp"
#include "minimal/agent/session.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/periodic_task.hpp"
#include "minimal/common/model_ref.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/agent/host_registries.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "minimal/config/policy.hpp"

namespace minimal::agent {

// Thrown by Create/CreateResolved when params.session_id names a session that
// is already open -- or is being constructed RIGHT NOW by a concurrent call.
// The registry reserves the id under its mutex before any construction work
// runs, so the loser fails BEFORE the transcript load, the torn-tail repair
// append, or any other side effect. The RPC layer maps this to ALREADY_EXISTS
// (a duplicate resume is the client's doing) rather than FAILED_PRECONDITION
// (which it reserves for configuration errors).
class SessionAlreadyOpen : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SessionRegistry {
public:
    // S5 lifetime policy, settable after construction (main reads settings
    // once and wires the idle-exit halves it owns). Every duration here is a
    // settings.json value (SessionSettings / ServerSettings) with a
    // documented default; nothing at the use site is hardcoded.
    struct ReaperConfig {
        // The never-attach guard (sessions.initial_attach_guard_seconds).
        std::chrono::milliseconds initial_attach_guard = config::kDefaultInitialAttachGuard;
        // A fully quiescent detached session (no subscribers, no turn in
        // flight, no live background tasks, no subagent children) is
        // terminated after being quiescent this long. Collecting a quiescent
        // session is lossless: the transcript stays in SQLite, and re-attach
        // becomes resume. 0 = never reap detached sessions.
        std::chrono::seconds detached_idle_ttl = config::kDefaultDetachedIdleTtl;
    };

    // Fills a session's transport-facing SessionParams -- transport, permission
    // checker (advisor-composed when one was selected), confirmation and ask
    // brokers -- once per session, after unattended_prompt_timeout resolved.
    // Production passes rpc::FinishSessionWiring; agent/ never names rpc/.
    using SessionWiring = std::function<void(SessionParams&)>;

    // Everything the process wires once at startup, injected at construction so a
    // registry is complete (and its configuration immutable) from the moment it
    // exists -- these used to be six post-construction setters that main called
    // one by one, in an order nothing enforced, while tests flipped them mid-life.
    struct Options {
        // S5 lifetime policy (sessions.* / server.* settings; nothing at the use
        // site is hardcoded).
        ReaperConfig reaper;
        // The project-scoped half of the command catalog. Optional: unset means
        // every session's project_commands is null, and Dispatch never sees a
        // project registry -- global commands work exactly as before.
        const commands::ProjectCommandSource* project_commands = nullptr;
        // The process-wide signal router, so every path that removes a session
        // from the map -- Terminate, TerminateAll, the reaper's sweeps -- can also
        // erase its subscription entry (otherwise a normally-terminated session
        // left its entry behind for the life of the process). Optional: unset
        // (tests) means no signal cleanup runs.
        SignalRouter* signal_router = nullptr;
        // small_model (settings.json): a <provider>/<model> reference resolved
        // ONCE against providers.json and the shared provider cache and stamped
        // into the loop config, so every session compacts on it. Empty or
        // unresolvable = compact on each session's own model (a failure is a
        // warning, never fatal: losing the cheap summarizer is a cost regression,
        // not a broken harness).
        std::string small_model;
        // sessions.unattended_prompt_timeout_seconds, applied through the same
        // CreateResolved funnel. Zero means every prompt waits forever.
        std::chrono::milliseconds prompt_timeout{0};
    };

    static Options GuardOnly(std::chrono::milliseconds initial_attach_guard) {
        Options options;
        options.reaper.initial_attach_guard = initial_attach_guard;
        return options;
    }

    // `registries`: tools, providers and observers are required (throws
    // std::invalid_argument when null). commands, compaction_strategies,
    // context_contributors and hooks are optional: null means sessions get no
    // command surface, only the built-in compaction, no contributed context
    // sections, and no hook dispatch. `wire_session` is required.
    SessionRegistry(const AgentPresetRegistry& presets, SessionWiring wire_session,
                    const plugins::HostRegistries& registries, session::SessionDatabase& db,
                    LoopConfig loop_config, common::Logger& logger, Options options);
    // Default options (a nested type's default member initializers cannot appear
    // in a default argument of its enclosing class's constructor).
    SessionRegistry(const AgentPresetRegistry& presets, SessionWiring wire_session,
                    const plugins::HostRegistries& registries, session::SessionDatabase& db,
                    LoopConfig loop_config, common::Logger& logger)
        : SessionRegistry(presets, std::move(wire_session), registries, db,
                          std::move(loop_config), logger, Options{}) {}
    // For callers that vary only the never-attach guard (tests mostly).
    SessionRegistry(const AgentPresetRegistry& presets, SessionWiring wire_session,
                    const plugins::HostRegistries& registries, session::SessionDatabase& db,
                    LoopConfig loop_config, common::Logger& logger,
                    std::chrono::milliseconds initial_attach_guard)
        : SessionRegistry(presets, std::move(wire_session), registries, db,
                          std::move(loop_config), logger, GuardOnly(initial_attach_guard)) {}
    ~SessionRegistry();

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    // The session row must already exist in the database -- params.session_id
    // names it -- because the database is what mints session ids. See
    // SessionServiceImpl::CreateSession, which owns that ordering.
    //
    // Throws std::runtime_error naming the problem: an unknown preset, or a
    // providers.json entry whose `provider` kind no loaded plugin registered.
    // Both are configuration errors the caller must be able to report back as
    // such, not conditions to paper over with a null provider.
    std::shared_ptr<Session> Create(SessionParams params);

    // SESSION FORK: creates a new top-level session whose store was seeded by
    // SessionDatabase::ForkSession (the caller or this method owns that call
    // ordering -- here the fork transaction runs first, then the session is
    // created through Create with resume=true, so the existing resume machinery
    // seeds the working set: [summary + tail] via a copied compaction
    // checkpoint, occupancy reseeding, and the pre-first-call compact).
    //
    // The fork point is the source's active transcript as it stands
    // (SessionDatabase::ForkSession). A non-empty instruction is submitted as
    // the fork's first user turn.
    //
    // agent_preset / model / run_mode override the SOURCE's values when set;
    // unset means the fork runs exactly what the source was running. Refuses a
    // subagent source (the store does, atomically). Throws the same error
    // families as Create; on a throw AFTER the fork transaction committed, the
    // minted child row is deleted so a failed fork leaves nothing behind.
    std::shared_ptr<Session> ForkSession(const std::string& source_session_id,
                                         const std::string& name, const std::string& instruction,
                                         const std::optional<config::ModelRef>& model,
                                         const std::optional<std::string>& agent_preset,
                                         std::optional<tools::PermissionRunMode> run_mode);

    // Same as Create(), but the caller supplies the ALREADY RESOLVED preset
    // instead of having it looked up by params.agent_preset. Subagent spawning
    // is the consumer: the spawner adapts the resolved preset first (is_subagent
    // set, prompts/subagent.md prefixed onto the system prompt, allowed lists
    // intersected with the parent's), and no name-based lookup could express
    // that. params.agent_preset still names the preset for the database row and
    // for SessionInfo.
    //
    // max_turns_override bounds the child's tool rounds (a subagent's whole life
    // is one RunTurn); nullopt keeps the registry-wide LoopConfig value, which
    // is 0 -- unbounded -- unless the spawner says otherwise.
    std::shared_ptr<Session> CreateResolved(SessionParams params, AgentPreset preset,
                                            std::optional<int> max_turns_override = std::nullopt);

    std::shared_ptr<Session> Find(const std::string& session_id) const;

    // The model -> provider path every session uses. Borrowed by
    // SessionSupportService so a plugin-configured model lands on the same
    // shared instances. The registry outlives every session and service call.
    ProviderResolver& resolver() { return resolver_; }
    const AgentPresetRegistry& presets() const { return presets_; }

    // Sessions of `parent_id` that are still RUNNING: neither terminated nor
    // finished-with-their-seeded-turn. This is the count the subagent
    // concurrency limit admits against -- a finished child whose tab is still
    // open holds no slot. Called under SubagentManager::limit_mu_ (lock order
    // limit_mu_ -> mutex_, one-directional; see subagent_manager.hpp).
    std::size_t ChildCount(const std::string& parent_id) const;

    // Terminates and forgets. Safe for an id that is already gone.
    void Terminate(const std::string& session_id);

    void TerminateAll();

    // Stops the reaper thread, exactly as the destructor does. Idempotent --
    // safe to call twice, and the destructor stays safe after an explicit
    // stop. Lets an exit cascade stop reaping (and so session termination
    // from the reaper) before it starts tearing down what sessions use.
    void StopReaper();

    // Whether any session still keeps the process up: a never-attached one
    // (its attach guard owns it) or one that is attached or busy (a turn in
    // flight, a live background task, subagent children). Every
    // detached-quiescent session is exit-transparent. What the server's idle
    // exit (rpc::ServerHousekeeping) asks.
    bool AnySessionHolds() const;

    std::size_t size() const;

private:
    // One reaper sweep: the never-attach guard, then the detached-idle TTL.
    void ReapOnce();
    std::chrono::milliseconds ReapInterval() const;
    std::unordered_map<std::string, std::size_t> LiveChildCountsLocked() const;
    bool DetachedQuiescentLocked(
        const std::string& session_id, const Session& session,
        const std::unordered_map<std::string, std::size_t>& live_child_counts) const;
    // The lock-already-held twin of ChildCount(), for callers that already
    // hold mutex_: ChildCount() would relock a non-recursive mutex on the
    // same thread and deadlock.
    std::size_t ChildCountLocked(const std::string& parent_id) const;

    // --- CreateResolved's builder steps (Phase 5 step 5) ----------------------
    // Split out of one ~250-line function; CreateResolved is now the ~15-line
    // sequence that calls these in order. Each keeps the exact behavior (and
    // most of the comments) the inline block it replaced had -- this is a
    // structural split, not a behavior change.
    // One notice to a new top-level session whose project's own agents, skills
    // and commands are being ignored for want of /trust-directory.
    void AnnounceIgnoredProjectContent(Session& session) const;
    void AssembleContext(SessionParams& params, const AgentPreset& preset) const;
    // `pending_repair`, when set on return, is a synthesized tool-result
    // repair message already folded into the returned seed's history but NOT
    // yet persisted -- the caller must append it via db_.AppendMessage only
    // after the session those results belong to has been fully constructed
    // (and ideally published). Persisting it here, before construction can
    // still fail, left an orphaned repair message in the transcript of a
    // session that was never actually created on a resume that threw.
    ResumeSeed LoadResumeSeed(const std::string& session_id,
                             std::optional<llm::Message>& pending_repair);
    // Phase 5 step 3: presets/commands/provider factories/instance cache used
    // to be set on the live Session AFTER construction (WireSession, set_*
    // calls) -- a window during which the worker thread already existed but
    // these were still null. Now constructor-injected: this fills the
    // SessionParams fields Session's ctor reads, before Session exists.
    void ApplyConstructorWiring(SessionParams& params);
    // The locked "findable" section: signal subscriptions, then the sessions_
    // insertion, in the same critical section (see the call site for why).
    void PublishSession(const std::string& session_id, const std::shared_ptr<Session>& session,
                        SignalRouter* signal_router, const std::vector<std::string>& signal_selectors,
                        bool signal_resume);

    const AgentPresetRegistry& presets_;
    SessionWiring wire_session_;
    // Process-wide, borrowed; see the constructor for which may be null.
    plugins::HostRegistries registries_;
    const commands::ProjectCommandSource* project_commands_ = nullptr;
    SignalRouter* signal_router_ = nullptr;
    tools::ToolRegistry& tools_;
    ProviderResolver resolver_;
    SessionObserverRegistry& observers_;
    session::SessionDatabase& db_;
    // Track M: the one SessionReader every session (top-level, resume, and
    // SubagentManager spawns alike) reads its transcript through --
    // stateless (holds only db_, every method takes session_id), so one
    // registry-lifetime instance suffices where CreateResolved used to build
    // a fresh one per session plus a second stack-local one for resume.
    std::shared_ptr<session::SessionReader> history_reader_;
    LoopConfig loop_config_;
    std::chrono::milliseconds prompt_timeout_{0};
    common::Logger& logger_;
    std::chrono::milliseconds initial_attach_guard_;
    ReaperConfig reaper_config_;
    void ApplySmallModel(const std::string& provider_model);
    // When each session was FIRST observed detached-and-quiescent (the
    // reaper-local half of the idle-TTL rule; the session itself carries no
    // timestamps for it). Erased whenever a session is active or attached.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> quiescent_since_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    // Ids claimed by an in-flight Create/CreateResolved, before any
    // construction work has run. The old guard was a check at the END of
    // construction -- after the resume transcript load, the torn-tail repair
    // append, and the worker start -- so two concurrent resumes both did the
    // full work before the loser was refused. A reservation under the same
    // mutex makes the refusal the FIRST thing that happens.
    std::unordered_set<std::string> reserved_;
    // Declared last so it is destroyed first: its thread reads everything
    // above.
    std::unique_ptr<common::PeriodicTask> reaper_;
};

}  // namespace minimal::agent
