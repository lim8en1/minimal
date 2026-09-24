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
#include "minimal/common/model_ref.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/session/session_database.hpp"
#include "minimal/session/session_reader.hpp"
#include "minimal/tools/tool_registry.hpp"

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
        std::chrono::milliseconds initial_attach_guard = std::chrono::seconds(10);
        // A fully quiescent detached session (no subscribers, no turn in
        // flight, no live background tasks, no subagent children) is
        // terminated after being quiescent this long. Collecting a quiescent
        // session is lossless: the transcript stays in SQLite, and re-attach
        // becomes resume. 0 = never reap detached sessions.
        std::chrono::seconds detached_idle_ttl = std::chrono::seconds(1800);
        // Once zero clients are attached AND every remaining session is
        // detached-quiescent (the reaper's own definition: idle, no live
        // background tasks, no subagent children; never-attached sessions
        // are owned by the attach guard instead), fire on_idle_expired
        // after this linger. 0 = never (daemon deployment). A
        // detached-but-busy session keeps the process up until its work
        // finishes; the TUI's Ping heartbeat keeps it up regardless while
        // any client is running.
        std::chrono::seconds idle_shutdown = std::chrono::seconds(5);
        // The service's attached-stream count; empty disables the idle exit.
        std::function<std::size_t()> attached_clients;
        // How long since a live client last made itself known -- the age of
        // the service's most recent Ping (see PingRequest). The idle exit
        // holds only while this too has exceeded idle_shutdown: a client that
        // is alive but holds nothing -- a TUI on its empty-state welcome
        // view, before its first session -- attaches no stream and creates no
        // session, and without this signal the reaper would read that silence
        // as "nobody is here" and shut the server down under the user.
        // Duration::max() (or any value >= idle_shutdown) means "no recent
        // sign of life". Empty disables this half (the count/session halves
        // alone decide), which keeps the reaper's tests and any pre-Ping
        // service on the old behavior.
        std::function<std::chrono::milliseconds()> client_idle_for;
        // Wired to Server::RequestShutdown by main. Fired from the reaper
        // thread; must not block.
        std::function<void()> on_idle_expired;
        // Called on every reaper tick, after the sweep, outside the registry
        // mutex -- production wires this to
        // SessionDatabase::MaybeRunPassiveCheckpoint() (wal_autocheckpoint=0
        // moved checkpoint I/O off the append path onto this thread, the
        // process's periodic one). A callback, not a concrete
        // SessionDatabase&, so the registry itself does not need a real WAL
        // to be reaper-testable. Empty (the default) means no periodic work.
        std::function<void()> periodic_checkpoint;
    };

    SessionRegistry(const AgentPresetRegistry& presets, tools::ToolRegistry& tools,
                    const llm::ProviderFactoryRegistry& providers,
                    SessionObserverRegistry& observers, session::SessionDatabase& db,
                    LoopConfig loop_config, common::Logger& logger,
                    std::chrono::milliseconds initial_attach_guard = std::chrono::seconds(10));
    ~SessionRegistry();

    // Replaces the lifetime policy before any session exists (or after -- the
    // reaper reads it under the mutex each tick). The constructor's
    // initial_attach_guard parameter stays for call sites that never touch
    // this; setting it overrides the guard too.
    void set_reaper_config(ReaperConfig config);

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
    // Optional: sessions created without one answer every command invocation
    // with "not available". Set before any session exists.
    void set_commands(const commands::CommandRegistry* commands) { commands_ = commands; }

    // The strategy registry every session's loop resolves a preset-declared
    // `compaction_strategy:` frontmatter against at compaction time. Optional
    // and borrowed, process-wide, read-only after plugin load (main.cpp); null
    // degrades to the settings-level strategy / built-in, so plain tests skip
    // it.
    void set_compaction_strategies(const CompactionStrategyRegistry* strategies) {
        compaction_strategies_ = strategies;
    }

    // The project-scoped half of the catalog (ProjectCommandSource). Same
    // optionality rule as set_commands: unset means every session's
    // project_commands is null, and Dispatch simply never sees a project
    // registry to consult -- global commands work exactly as before.
    void set_project_commands(const commands::ProjectCommandSource* project_commands) {
        project_commands_ = project_commands;
    }

    // The process-wide signal router (one instance, wired from main.cpp),
    // so every path that removes a session from sessions_ -- Terminate,
    // TerminateAll, and the reaper's two sweeps -- can also erase its
    // subscription entry. Without this, subs_ only ever shrank on a RESUME
    // (SignalRouter::Unsubscribe was called from PublishSession alone); a
    // session that terminates normally (the common case, especially for
    // short-lived subagents) left its subs_ entry behind for the life of
    // the process. Same optionality rule as set_commands: unset (tests)
    // means no signal cleanup runs, matching today's behavior exactly.
    void set_signal_router(SignalRouter* signal_router) { signal_router_ = signal_router; }

    // Plugin context contributors (context_contributor.hpp), asked once per
    // session in CreateResolved -- the same funnel as project context. Unset
    // (tests) means no contributed sections, matching set_signal_router's
    // optionality rule.
    void set_context_contributors(const ContextContributorRegistry* contributors) {
        context_contributors_ = contributors;
    }

    // Project context (AGENTS.md/CLAUDE.md) settings, read once at startup.
    // Optional but DEFAULTS TO DISABLED: until main wires the parsed settings
    // in, no session reads anything from its project_root -- the same shape as
    // set_commands, where a missing producer is a feature that is off rather
    // than one running on guessed defaults. Loading happens per session in
    // CreateResolved -- the one funnel top-level creation, resume, and
    // SubagentManager spawns share -- because the file lives in the SESSION's
    // project_root, not in any preset.
    void set_project_context(config::ProjectContextSettings settings) {
        project_context_ = settings;
    }

    // small_model (settings.json): resolves the <provider>/<model> reference
    // ONCE against providers.json and the shared provider cache, and stamps
    // the result into loop_config_ so every session compacts on it. Unset or
    // unresolvable = compact on each session's own model (a failure here is a
    // warning, never fatal: losing the cheap summarizer is a cost regression,
    // not a broken harness).
    void set_small_model(const std::string& provider_model) {
        std::string error;
        auto resolved = resolver_.ResolveLlm(provider_model, error);
        if (!resolved) {
            common::DefaultLogger().warn(
                "settings.small_model {}; compaction uses each session's own model", error);
            return;
        }
        const config::ResolvedModel& model = *resolved->model;
        LoopConfig::SmallModel small;
        small.provider = std::move(resolved->provider);
        small.model_name = model.provider.model_name;
        small.parameters = model.parameters;
        small.max_tokens = model.max_tokens;
        small.context_window_tokens = model.context_window_tokens;
        loop_config_.small_model = std::move(small);
    }

    // sessions.unattended_prompt_timeout_seconds, read once at startup like
    // project_context above and applied through the same CreateResolved
    // funnel. Zero (the default) means every prompt waits forever, unchanged
    // from before S5.
    void set_prompt_timeout(std::chrono::milliseconds timeout) {
        prompt_timeout_ = timeout;
    }

    // The process-wide hook dispatcher (track O). Optional and borrowed; set
    // before any session exists. Attached to every LoopConfig::hooks that
    // arrives null -- the funnel property, like the history reader below.
    void set_hook_dispatcher(const hooks::HookDispatcher* hooks) { hooks_ = hooks; }

    std::shared_ptr<Session> Create(SessionParams params);

    // SESSION FORK: creates a new top-level session whose store was seeded by
    // SessionDatabase::ForkSession (the caller or this method owns that call
    // ordering -- here the fork transaction runs first, then the session is
    // created through Create with resume=true, so the existing resume machinery
    // seeds the working set: [summary + tail] via a copied compaction
    // checkpoint, occupancy reseeding, and the pre-first-call compact).
    //
    // agent_preset / model / run_mode override the SOURCE's values when set;
    // unset means the fork runs exactly what the source was running. Refuses a
    // subagent source (the store does, atomically). Throws the same error
    // families as Create; on a throw AFTER the fork transaction committed, the
    // minted child row is deleted so a failed fork leaves nothing behind.
    std::shared_ptr<Session> ForkSession(const std::string& source_session_id,
                                         std::int64_t target_seq, const std::string& name,
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
    // stop. Exists so a caller whose exit cascade destroys the objects the
    // ReaperConfig callbacks capture (service, server) BEFORE this registry
    // can quiesce the reaper first: TerminateAll does not touch the reaper,
    // and without this the ~250ms tick would evaluate those by-reference
    // captures on already-destroyed objects while its destructor still waits
    // behind theirs in reverse destruction order.
    void StopReaper();

    std::size_t size() const;

private:
    void ReaperMain();
    // The lock-already-held twin of ChildCount(), for ReaperMain -- which
    // holds mutex_ across its whole sweep. Calling the public ChildCount()
    // from there would relock a non-recursive std::mutex on the same thread
    // and deadlock the reaper permanently, wedging every subsequent Find()/
    // size()/etc. behind a lock nobody will ever release.
    std::size_t ChildCountLocked(const std::string& parent_id) const;

    // --- CreateResolved's builder steps (Phase 5 step 5) ----------------------
    // Split out of one ~250-line function; CreateResolved is now the ~15-line
    // sequence that calls these in order. Each keeps the exact behavior (and
    // most of the comments) the inline block it replaced had -- this is a
    // structural split, not a behavior change.
    void ApplyProjectContext(SessionParams& params) const;
    void AppendSignalContext(SessionParams& params) const;
    void AppendContributedContext(SessionParams& params, const AgentPreset& preset) const;
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
    // Optional and borrowed; every session gets the same one.
    const commands::CommandRegistry* commands_ = nullptr;
    const commands::ProjectCommandSource* project_commands_ = nullptr;
    SignalRouter* signal_router_ = nullptr;
    const ContextContributorRegistry* context_contributors_ = nullptr;
    tools::ToolRegistry& tools_;
    ProviderResolver resolver_;
    SessionObserverRegistry& observers_;
    // Optional and borrowed; every session gets the same one via the
    // CreateResolved funnel.
    const hooks::HookDispatcher* hooks_ = nullptr;
    session::SessionDatabase& db_;
    // See set_compaction_strategies. Borrowed, process-wide, optional.
    const CompactionStrategyRegistry* compaction_strategies_ = nullptr;
    // Track M: the one SessionReader every session (top-level, resume, and
    // SubagentManager spawns alike) reads its transcript through --
    // stateless (holds only db_, every method takes session_id), so one
    // registry-lifetime instance suffices where CreateResolved used to build
    // a fresh one per session plus a second stack-local one for resume.
    std::shared_ptr<session::SessionReader> history_reader_;
    LoopConfig loop_config_;
    config::ProjectContextSettings project_context_ = [] {
        config::ProjectContextSettings disabled;
        disabled.enabled = false;
        return disabled;
    }();
    std::chrono::milliseconds prompt_timeout_{0};
    common::Logger& logger_;
    std::chrono::milliseconds initial_attach_guard_;
    ReaperConfig reaper_config_;
    // When each session was FIRST observed detached-and-quiescent (the
    // reaper-local half of the idle-TTL rule; the session itself carries no
    // timestamps for it). Erased whenever a session is active or attached.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> quiescent_since_;
    // When the process first observed zero clients and zero sessions.
    std::optional<std::chrono::steady_clock::time_point> idle_since_;

    mutable std::mutex mutex_;
    std::condition_variable reaper_cv_;
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
    // Ids claimed by an in-flight Create/CreateResolved, before any
    // construction work has run. The old guard was a check at the END of
    // construction -- after the resume transcript load, the torn-tail repair
    // append, and the worker start -- so two concurrent resumes both did the
    // full work before the loser was refused. A reservation under the same
    // mutex makes the refusal the FIRST thing that happens.
    std::unordered_set<std::string> reserved_;
    bool stopping_ = false;
    std::thread reaper_;
};

}  // namespace minimal::agent
