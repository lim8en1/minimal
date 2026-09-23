#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <functional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_compactor.hpp"
#include "minimal/agent/injection_queue.hpp"
#include "minimal/agent/session_observer.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/settings_config.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/stream_event.hpp"
#include "minimal/llm/token_accounting.hpp"
#include "minimal/session/history_store.hpp"
#include "minimal/session/history_window.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_pool.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::agent {

struct LoopConfig {
    bool auto_compact = true;
    // When to compact. Never (the default) means compaction only ever runs on
    // explicit /compact. Window-relative strategies resolve against the active
    // preset's context_window_tokens at trigger time -- see compaction_trigger.
    config::CompactionTriggerSettings compaction_trigger;
    std::int64_t compaction_keep_recent_tokens = 0;
    // settings.json memory.compaction_replace_percent. When keep_recent is 0,
    // Compact() turns this into a keep-recent token budget of
    // (100 - percent)% of the working history's estimate, so the cut is by
    // token weight rather than message count. 0 (the default for hand-built
    // configs) keeps the legacy history.size()/2 halving.
    int compaction_replace_percent = 0;
    // Put record_summary on every main request, so the structured
    // summarizer's request (the main request + one instruction) keeps the
    // tools array -- and the prompt cache -- intact. main.cpp turns it on;
    // off for hand-built configs, whose structured summarizer then appends the
    // tool to its own request (a cache miss, nothing worse).
    bool offer_summary_tool = false;
    int max_retries = 3;
    int max_turns = 0;  // 0 = unbounded; subagents bound their own life

    // Parallel tool dispatch width (ARCHITECTURE.md "Tool dispatch"): how many
    // tool calls of one round may execute concurrently. 1 is exactly the
    // sequential behavior -- the ToolPool degenerates to inline execution and
    // starts no threads. Sourced from settings.json's
    // tools.max_parallel_tool_calls.
    int max_parallel_tool_calls = 4;

    // The global tool-input ceiling (settings.json tools.max_input_bytes).
    // Checked at the gate, before Prepare: no hook payload, no permission
    // request, no execution -- one error result the model adapts to.
    std::int64_t max_tool_input_bytes = 10 * 1024 * 1024;

    // The global tool-RESULT ceiling (settings.json tools.max_result_bytes).
    // Mirrors max_tool_input_bytes on the output side: a core tool like bash
    // caps its own output, but that cap lives in the plugin, not the harness
    // -- an MCP tool or a third-party plugin tool has no ceiling at all
    // short of this one. Checked once, right after Execute() returns, so it
    // applies uniformly to every tool regardless of who wrote it. 0 disables.
    std::int64_t max_tool_result_bytes = 10 * 1024 * 1024;

    // Ceiling on a result's display payload (settings.json
    // tools.max_display_bytes). Over it the payload is dropped whole. 0 disables.
    std::int64_t max_tool_display_bytes = 128 * 1024;

    // Exponential-with-EQUAL-jitter backoff between retries: half the ceiling
    // is a fixed floor, the other half random. Full jitter can legally draw
    // zero, which leaves a real 429 unbackoffed and makes the wait
    // unassertable. Zero base delay disables the wait, which is what the tests
    // that assert on attempt COUNTS want -- they would otherwise pay the delay
    // for nothing.
    std::chrono::milliseconds retry_base_delay{500};
    std::chrono::milliseconds retry_max_delay{8000};

    // The process-wide hook dispatcher (track O). Borrowed; null in tests that
    // do not exercise hooks -- both compaction emissions are observe-only
    // EmitAsync calls and become no-ops. Owned by main, shared by every
    // session, read-only after Configure.
    const hooks::HookDispatcher* hooks = nullptr;

    // S4: the compaction strategy, resolved ONCE in main.cpp by name
    // (memory.compaction_strategy) via CompactionStrategyRegistry and copied
    // into every session's LoopConfig whole -- the strategy object survives
    // to the consumer, rather than dissolving into two loose std::functions
    // at the producer. Default-constructed (every LoopConfig a test builds by
    // hand, and any AgentLoop built directly with its own `compactor`
    // constructor argument) falls back to today's production behavior
    // exactly: an unset cut_selector means AgentLoop::Compact() keeps the
    // legacy history.size()/2 halving; EffectiveCompactor() below is what
    // Session actually passes to AgentLoop's ctor. Neither half ever bypasses
    // snap_compaction_boundary, which AgentLoop::Compact() applies
    // unconditionally regardless of where the candidate cut came from.
    CompactionStrategy compaction_strategy;

    // small_model (opencode compat): an alternate <provider>/<model> for the
    // SUMMARIZER call only -- never the session's own turns. Resolved once by
    // SessionRegistry at startup (settings.small_model against providers.json
    // + the shared provider cache) and copied into loop_config_ whole, so
    // every session compacts on the cheap model without per-turn lookups.
    // Unset = compact on the session's own provider, model and settings.
    struct SmallModel {
        std::shared_ptr<llm::LlmProvider> provider;
        std::string model_name;
        nlohmann::json parameters = nlohmann::json::object();
        std::optional<int> max_tokens = std::nullopt;
    };
    std::optional<SmallModel> small_model;

    // memory.estimate_zero_usage: when a turn's backend usage is all-zero
    // (the OpenAI-compatible proxy gap), seed occupancy from the request's
    // bytes/4 estimate so the compaction trigger arms, and flag the turn's
    // wire usage as estimated. Estimates never reach cumulative totals and
    // never feed cost pricing.
    bool estimate_zero_usage = true;

    // Per-provider-round usage signal, fired the moment each response lands
    // (NOT at turn end) -- the seam a cost tracker uses to keep a RUNNING
    // total visible while the turn is still working, subagent runs included.
    // Session wires this to SessionObserverRegistry::NotifyProviderRound; null
    // in tests that construct a bare AgentLoop. Fired for estimated rounds
    // too, flagged -- a consumer must record the round's existence without
    // pricing its tokens (RoundUsage::usage_estimated). Runs on the session
    // worker thread inside the turn; the callback must not block (the same
    // fire-and-forget dispatch contract every observer notification has).
    std::function<void(RoundUsage)> on_provider_round;

    // The producer-resolved summarizer. The settings-level strategy rides
    // `compaction_strategy` above (resolved once in main.cpp); an AGENT
    // PRESET may override the format per agent by declaring
    // `compaction_strategy:` frontmatter -- AgentPreset carries the NAME
    // (config code cannot see the strategy registry), and this resolves it
    // against the registry the LoopConfig holds no reference to, so the
    // registry is passed in per call.
    //
    // Precedence, first hit wins:
    //   1. the test/direct-API injection seam (AgentLoop's `compactor` ctor
    //      argument -- AgentLoop consults it BEFORE this, see Compact());
    //   2. preset.compaction_strategy naming a REGISTERED kind -- an unknown
    //      name (typo, or the providing plugin is disabled) warns once and
    //      falls through: a typo'd agent file must not brick compaction for
    //      every session running it;
    //   3. the settings-level strategy's summarize half;
    //   4. compact_messages, the built-in free-text compactor -- the
    //      historical final fallback.
    //
    // Centralized here so the consumer (Session's ctor, building AgentLoop)
    // does not need to know what the fallback is. Registry may be null (tests
    // that never built one): steps 2 is skipped, 3/4 still apply.
    Compactor EffectiveCompactor(const AgentPreset& preset,
                                 const CompactionStrategyRegistry* strategies) const {
        if (!preset.compaction_strategy.empty() && strategies != nullptr) {
            const std::string_view name = preset.compaction_strategy;
            if (const auto* found = strategies->Find(name)) {
                if (found->summarize) return found->summarize;
            }
        }
        return compaction_strategy.summarize ? compaction_strategy.summarize
                                             : Compactor(compact_messages);
    }
};

// Brackets one tool invocation, surfaced to the client as ToolExecutionEvent.
// Distinct from a permission request, which BLOCKS and needs an answer: this is
// a non-gated status push, so a client can show "Running bash..." for a call
// that was never gated at all.
//
// Carries no summary line. Producing one means asking the tool to Disclose(),
// which for bash resolves argv[0] against the session PATH -- worth paying when
// a human is about to be asked a question, not on every call of an
// already-allowed tool. Rendering from tool_name + input is the client's job.
struct ToolLifecycleEvent {
    std::string tool_use_id;
    std::string tool_name;
    nlohmann::json input;
    bool started = false;
    bool is_error = false;  // meaningful only when started == false
    std::int64_t started_at_unix_ms = 0;
    // Wall time the call took, set on the FINISH event only. SessionObserver's
    // ToolInvocation carries a duration and this is the only place that knows
    // it; the wire event derives its own from the start timestamp.
    std::chrono::milliseconds duration{0};
};

// One successful compaction, surfaced to the client as a CompactionEvent and
// persisted beside the checkpoint so scrollback can render it after resume.
// boundary_seq is the last transcript seq the summary replaced (-1 when the
// loop has no persisted mapping -- direct-API loops). Token figures are the
// whole-context occupancy before and the bytes/4 estimate after.
struct CompactionEvent {
    std::int64_t at_seq = -1;  // the last persisted seq when it ran
    std::int64_t boundary_seq = -1;
    std::int64_t folded_messages = 0;
    std::int64_t tokens_before = 0;
    std::int64_t tokens_after = 0;
    std::string summary_text;
    std::string trigger;  // "auto" | "manual"
    std::int64_t created_at_unix_ms = 0;
    // A guard's message: why a compaction was skipped (skipped = true: nothing
    // was folded, no summary) or why a completed one looks ineffective.
    std::string warning;
    bool skipped = false;
};

// Session-scoped, as distinct from the preset (which is switchable via /agent)
// and the config (which is tuning). project_root in particular is per-session,
// not server-global: it is the boundary for path confinement, @-mentions, and
// bash's initial cwd.
struct SessionContext {
    explicit SessionContext(common::Logger& logger) : logger(logger) {}

    std::string session_id;
    std::filesystem::path project_root;

    // Incremental tool output. Separate from StreamEventSink because tool output
    // is not part of the LLM stream -- it reaches the client as its own
    // ToolOutputDeltaEvent. Left unset until the RPC layer wires it.
    std::function<void(std::string_view tool_use_id, std::string_view chunk)> tool_output_sink;

    // Start/finish brackets for each tool call. Optional: unset in tests that
    // only care about the transcript.
    std::function<void(const ToolLifecycleEvent&)> tool_lifecycle_sink;

    // Fired once per successful compaction, on the worker, after the fold.
    // Optional: unset in tests that only care about the working set.
    std::function<void(const CompactionEvent&)> compaction_sink;

    // Handed to every ToolContext this session builds. Session-scoped rather than
    // a free function so a later per-session sink (session-id prefix) is an
    // implementation detail of the wiring, not of the tools.
    common::Logger& logger;

    // The real-time steering gate, applied at flush time to every USER-TEXT
    // injection (a mid-turn UserChatTurn routed here by Session). Runs on the
    // worker -- the flush is worker-only -- which is where the
    // core.user_prompt_submit subprocess budget is allowed to land, matching
    // the turn-pop veto seam. Returning false DROPS the message (it is never
    // persisted; the gate itself surfaces the reason on the wire). Null means
    // ungated: plain test loops that never wired hooks.
    std::function<bool(const llm::Message&)> steer_gate;

    // The session's cross-thread staging queue for out-of-band messages
    // (non-blocking subagent completions, background task results). Shared
    // because the producers are foreign threads whose handles must stay safe
    // past this loop's death: the queue lives in the session's shared channels
    // block, and Push refuses after Terminate closes it. Null in plain tests
    // that never inject -- the loop then creates a private one, which is
    // behaviorally identical for a loop nothing else talks to.
    std::shared_ptr<InjectionQueue> injections;

    // --- Deferred-completion wiring, copied into every ToolContext -----------
    //
    // All three are lifetime-safe (backed by the session's shared channels
    // block), which is the whole point: a tool capturing them beyond its own
    // invocation -- background bash's watcher thread -- degrades cleanly after
    // the session dies instead of dangling. See tools/tool.hpp for the
    // contract each one carries.

    // Stages a message for injection at a turn boundary; false = terminating.
    std::function<bool(llm::Message)> injection_sink;
    // Expires with the session; a watcher polls it and self-terminates.
    std::weak_ptr<const void> session_lifetime;
    // BackgroundTaskEvent lifecycle, mapped onto a wire event by the session.
    std::function<void(const tools::BackgroundTaskEvent&)> task_event_sink;

    // The interactive ask (ToolContext::ask_user), forwarded from the
    // session's AskBroker wiring. Null when the session has no ask broker
    // (see SessionParams::ask_broker) -- the tool-ask plugin's tool then
    // degrades to an error result instead of parking a worker nobody can
    // answer. Unlike the handles above this BLOCKS for a human, so it must
    // only run where tool execution already allows blocking.
    std::function<tools::AskAnswer(const tools::AskRequest&)> ask_user;

    // The session's tool-facing history window (see ToolContext::history). Owned
    // by the Session, which outlives this loop; null when the session's store is
    // not SQLite-backed (test fakes), which makes ctx.history null -- the
    // natural "unavailable" case, not an error path.
    const session::HistoryWindow* history = nullptr;
};

struct TurnResult {
    llm::StopReason stop_reason = llm::StopReason::Unknown;
    // Final assistant text. A subagent's whole result is this string, handed
    // back to the parent as its ToolResultBlock.
    std::string final_text;
    llm::Usage last_usage;
    int provider_calls = 0;
    bool hit_max_turns = false;
    // Every tool this turn invoked, in call order, duplicates included -- it is
    // a record of what happened, not a set. Feeds TurnSummary::tool_names_invoked,
    // which ARCHITECTURE.md has always specified and which was shipped
    // permanently empty because nothing populated it.
    std::vector<std::string> tool_names_invoked;
    // The model id this turn's provider calls were made with -- the request's
    // model, not the backend-reported one, so a per-model cost rate resolves
    // against the same name the user's config keys it under. Set as soon as the
    // first request is built, so a turn that failed or was cancelled before any
    // response still reports what it would have billed.
    std::string model;
    // True when last_usage is the request's bytes/4 estimate rather than a
    // backend-reported count (the zero-usage proxy gap). The wire mirrors it
    // on TurnCompletedEvent and TurnSummary carries it to observers -- cost
    // pricing keys off it to keep an estimated token count unpriced.
    bool usage_estimated = false;
    // The turn's CUMULATIVE usage: the sum across every reported provider
    // round (each round is a separately-billed request; `last_usage` is only
    // the final round's occupancy-shaped count). Estimated rounds contribute
    // nothing. Session copies this into TurnSummary::turn_usage for observers.
    llm::Usage turn_usage;
    // False when ANY round of the turn went unreported: the sum above is then
    // a lower bound and must be treated as unreported, not billed partially.
    bool turn_usage_fully_reported = true;
};

// RESUME: the stored transcript, loaded without being re-appended -- the
// store already holds it. Empty/default-constructed for a fresh session.
// The three fields must agree with each other (see first_persisted_seq
// below), which is exactly why they travel as one struct instead of three
// separately-threaded constructor arguments: Session and AgentLoop each used
// to take them as three trailing params that had to stay in sync by
// convention across two call sites.
struct ResumeSeed {
    // Construction-time seeding (rather than a later setter) means a turn can
    // never run against a half-initialized history. Seeded history also seeds
    // the occupancy estimate, so auto-compaction can fire BEFORE the first
    // post-resume provider call -- without that, a resumed session whose
    // stored context exceeds the window would fail the call outright.
    std::vector<llm::Message> history;
    // Where the seed's PERSISTED portion begins in the store's seq space --
    // what the compaction checkpoint's boundary_seq derives from. nullopt for
    // a fresh session (nothing persisted yet) or a seed with no persisted
    // messages. With a checkpoint seed, this is boundary_seq + 1 and
    // head_is_summary is true: the seed's first message is the synthetic
    // summary, which is not in the store. The INVARIANT that makes this one
    // number sufficient: the persisted messages in history_ are always a
    // contiguous suffix of the stored transcript, because appends continue at
    // MAX(seq)+1 and compaction only ever folds from the head.
    std::optional<std::int64_t> first_persisted_seq;
    bool head_is_summary = false;
    // With a checkpoint seed: the last seq the compaction KEPT verbatim (its
    // at_seq). Messages up to it were produced against the pre-fold prefix,
    // so the loop drops their thinking when the provider binds thinking to
    // its prefix -- exactly what the live fold did. Later messages were
    // produced after the fold, against the prefix a resume reproduces, and
    // keep theirs. -1 = none.
    std::int64_t kept_through_seq = -1;
};

// Runs on the session worker thread. Blocks freely -- permission waits, provider
// calls, tool subprocesses -- which is safe only because the gRPC reader thread
// is separate and keeps draining.
class AgentLoop {
public:
    AgentLoop(llm::LlmProvider& provider, tools::ToolExecutor& executor,
              tools::ToolRegistry& registry, session::HistoryStore& store, AgentPreset preset,
              LoopConfig config, SessionContext session, std::function<bool()> is_cancelled,
              // The pause probe (SetPausedEvent): checked ONLY at the loop top,
              // before each provider call. The tool round in flight when the
              // pause lands always completes and persists its real results --
              // the stop point is the tool-call return, never mid-round --
              // which is what keeps the transcript paired and the turn
              // resumable. Null in tests that never pause.
              std::function<bool()> is_paused,
              llm::StreamEventSink event_sink, Compactor compactor, ResumeSeed seed = {},
              // The rendered project-context section (AGENTS.md/CLAUDE.md),
              // loaded from project_root by the registry and carried on
              // SessionParams. Appended after the preset's system prompt on
              // every BuildChatRequest -- a SESSION property held here rather
              // than baked into the preset, so /agent re-resolving the preset
              // cannot silently drop the project's instructions.
              std::string project_context = {},
              // The strategy registry a preset's `compaction_strategy:`
              // frontmatter resolves against at each compaction attempt.
              // Borrowed, process-wide, read-only after plugin load; null
              // (plain tests) skips the preset-name step of the resolution.
              const CompactionStrategyRegistry* compaction_strategies = nullptr);

    // Appends `seed_message`, then runs to a natural stop. A subagent's entire
    // life is one call to this -- there is one loop implementation, not two.
    // Anything queued out-of-band while the session was idle is flushed BEFORE
    // the seed, so it precedes both the seed and the first provider call.
    TurnResult RunTurn(llm::Message seed_message);

    // The seedless turn: the wake path. When an injection lands on an IDLE
    // session and settings enabled the wake, the worker runs a turn with no
    // seed of its own -- the injected messages ARE the user turn. The loop
    // body is shared with RunTurn(seed); only the seed differs.
    TurnResult RunTurn();

    // A bare push onto the injection queue, flushed at a turn boundary rather
    // than injected out-of-band: landing a user message between a tool_use
    // and its tool_result is a hard 400 on both wire formats. Unrelated to
    // Session::QueueInjectedMessage (no terminated_ guard, no worker wakeup)
    // despite the similar name -- production callers go through Session,
    // which every real AgentLoop is wrapped by; this exists for tests that
    // construct a bare AgentLoop.
    bool PushInjectedMessage(llm::Message message);

    const std::vector<llm::Message>& history() const { return history_; }

    // True when the working history ends in an UNANSWERED user-text message
    // -- a real-time steering submission that was flushed after the turn's
    // last provider call (the model had already finished). The session
    // worker's wait predicate keys on this to run the seedless continuation
    // that answers it: keying on the injection QUEUE instead is wrong,
    // because the final turn-boundary flush already drained the queue. The
    // durable record is the transcript; "ends in user text" is exactly
    // "the model owes a reply". Worker-thread-only (history_ mutates there).
    bool HistoryEndsWithUnansweredUserText() const;

    // Whether the running turn has passed the point a mid-turn user
    // submission may steer from: its own user-visible content (the seed, or
    // the wake turn's top-of-loop flush) is in the history. Session's
    // SubmitTurn keys its steering routing on this -- before the seed lands,
    // a submission is a QUEUED turn, not steering: steering it would flush
    // ahead of the seed (RunTurn's turn-start flush precedes AppendAndPersist),
    // reversing the submission order the user typed. Atomic: the reader
    // thread's SubmitTurn polls it while the worker is inside RunTurn.
    bool OpenToSteering() const {
        return turn_open_to_steering_.load(std::memory_order_acquire);
    }
    llm::ChatRequest BuildChatRequest() const;

    // Occupancy estimate covering what a request will actually carry: the
    // working history PLUS the system prompt (preset body + project context)
    // and the tools array. estimate_tokens(history_) alone under-counted by
    // exactly what backend ground truth counts and a seed built from it
    // excluded -- so a resumed or compacted session under-triggered
    // compaction, contradicting what the seeding comments claimed. Worker
    // thread only (BuildChatRequest is); seed sites are rare, so building the
    // request's tool definitions here is the honest way to keep the estimate
    // and the request from drifting apart.
    std::int64_t EstimateContextTokens() const;

    // --- /-command seams. WORKER THREAD ONLY -------------------------------
    //
    // Safe without a lock because a command that touches session state is not
    // ReadOnly(), so it is queued and run by the worker BETWEEN turns -- the
    // same thread that reads all of this while running one. That is also what
    // makes SetProvider safe: the worker is provably not inside a provider call
    // at the moment it swaps the reference.

    // /compact. Compacts regardless of occupancy -- the user asked -- but still
    // respects the per-turn failure cooldown, which exists so a broken
    // summarizer is not re-invoked repeatedly, and is just as true when the
    // repeated invocation is manual. An optional `prompt_override` (the
    // resolved TEXT of a prompts/<name>.md, resolved by the caller through
    // AgentPresetRegistry::ResolveCompressionPrompt) replaces the preset's
    // compression prompt for this one compaction; it is not sticky, and
    // auto-compaction never sees it.
    //
    // Returns how many messages were folded away, or nullopt if nothing was.
    std::optional<std::size_t> CompactNow(std::string_view prompt_override = {});

    // /clear. Drops the in-memory working set; the STORE keeps the full record,
    // the same rule compaction follows. Scrollback is unaffected.
    void ClearHistory();

    // --- Session traversal (rewind). WORKER THREAD ONLY, like every other
    // seam above. The Session orchestrates: it marks the durable tail
    // discarded (or restores it, for a redo) and then calls one of the two
    // methods below to bring the in-memory working set in line. A turn can
    // never run against a half-migrated state because both run between turns,
    // on this thread.

    // The seq bound the synthetic summary head covers, when there is one:
    // everything with seq <= this is summarized in history_[0]. The same
    // number a compaction checkpoint was written with, so the rewind path can
    // tell "the summary still describes kept turns" from "the summary
    // describes turns that did not happen". nullopt when history_ has no
    // summary head (never compacted, or the checkpoint was cleared).
    std::optional<std::int64_t> CheckpointBoundary() const {
        if (head_is_summary_ && first_persisted_seq_.has_value()) {
            return *first_persisted_seq_ - 1;
        }
        return std::nullopt;
    }

    // Drops the working set from `target_seq` onward -- the memory half of a
    // rewind whose cut landed ABOVE the checkpoint boundary, so the synthetic
    // summary (when present) still describes only kept turns and survives.
    // Re-seeds the occupancy estimate from the kept set -- a stale count
    // describing discarded turns would fire the compaction trigger early --
    // and drops pending subagent completions, which belong to the tail.
    void TruncateAfter(std::int64_t target_seq);

    // The memory half of a rewind whose cut landed AT OR BELOW the checkpoint
    // boundary. There the summary over-covers turns that did not happen, and
    // dropping it wholesale would lose the still-active turns BELOW the cut
    // that it also summarized -- so the working set is rebuilt from the
    // store's ACTIVE prefix, which the Session loads and hands in here. The
    // checkpoint is a cache of the discarded view and the Session clears it;
    // this method replaces history_ wholesale, resets the seq bookkeeping to
    // the loaded prefix, and re-seeds the occupancy estimate. The load is the
    // resume-shaped cost: bounded by the active prefix, paid only on a
    // user-invoked rewind under a compaction boundary.
    void ReseedFromStore(std::vector<llm::Message> rows, std::int64_t first_seq);

    // The memory half of a REDO: the store restored rows [first_seq, ...] and
    // they join the working set WITHOUT being re-appended -- resume's seed
    // shape, applied to a live loop. Fixes first_persisted_seq_ when the
    // working set was empty (rewound to the transcript's start, then redone),
    // and re-seeds the occupancy estimate over the whole set.
    void RestorePersisted(std::vector<llm::Message> rows, std::int64_t first_seq);

    // /agent, /model. The preset carries the model, so both go through here.
    void SetPreset(AgentPreset preset) { preset_ = std::move(preset); }
    const AgentPreset& preset() const { return preset_; }

    // The allowed-AND-alive MCP server set, resolved by the session against the
    // McpClientManager at construction and at every /agent switch (a preset
    // change can re-scope the servers). A dead server's exclusion is exactly
    // this list: the model never sees tools for a server that is not in it.
    // WORKER THREAD ONLY, like SetPreset.
    void SetMcpServers(std::vector<std::string> servers) { mcp_servers_ = std::move(servers); }

    // The current turn's cancellation token, minted by the session when it
    // pops the turn. Handed to every ToolContext so blocking tools can wait
    // on its cv instead of polling. May be null between turns (tools cannot
    // observe that: they only run inside a turn). WORKER THREAD ONLY.
    void SetCancelToken(tools::CancelTokenPtr token) { cancel_token_ = std::move(token); }

    // /model across providers: the session builds the new provider and rebinds
    // it. A reference, not an owner -- the session owns the object.
    void SetProvider(llm::LlmProvider& provider) { provider_ = provider; }

private:
    // The system/tools half of BuildChatRequest, factored out so
    // EstimateContextTokens does not have to deep-copy the whole working
    // history (BuildChatRequest's request.messages = history_) just to read
    // request.system and request.tools -- for a 512k-1M-token session that
    // copy ran on every compaction check, every resume, and every rewind/
    // redo, exactly the RAM spike ARCHITECTURE.md's "the request body is
    // serialized, not materialized" section argues against. messages stays
    // empty; callers that need the full request still call BuildChatRequest.
    struct SystemAndTools {
        std::optional<std::string> system;
        std::vector<llm::ToolDefinition> tools;
    };
    SystemAndTools BuildSystemAndTools() const;

    // Drops ThinkingBlocks from history_[first, last) -- the messages a
    // compaction kept verbatim -- when the provider rejects replaying
    // thinking whose prefix changed (LlmProvider::BindsThinkingToPrefix).
    // Working set only; the transcript keeps them.
    void DropStaleThinking(std::size_t first, std::size_t last);

    // The estimate's real body, taking an already-built system/tools pair
    // instead of calling BuildSystemAndTools() itself. The public
    // EstimateContextTokens() delegates to this after building its own pair;
    // the zero-usage-estimate call inside RunTurnLoop calls it directly with
    // the SAME chat_request.system/chat_request.tools BuildChatRequest just
    // built a few lines earlier in the same tool round, instead of paying a
    // second DefinitionsFor/DefinitionsForMcpServers materialization and sort
    // for values it already has.
    std::int64_t EstimateContextTokens(const std::optional<std::string>& system,
                                       const std::vector<llm::ToolDefinition>& tools) const;

    void AppendAndPersist(llm::Message message, std::optional<int> output_tokens = std::nullopt);
    // Break-path repair: a turn that ends on Cancelled/MaxTokens/ContentFilter/
    // Refusal/StopSequence with the response's tool_use blocks already appended
    // would otherwise orphan them (hard 400 on every later turn). Synthesizes
    // error results for any unanswered calls, exactly as the resume path does.
    void RepairOrphanedToolUses(const std::vector<const llm::ToolUseBlock*>& tool_uses);
    void MaybeCompact();
    // The shared body: every guard except the occupancy trigger, which is what
    // MaybeCompact adds and CompactNow deliberately skips. `trigger` names who
    // asked -- the pre/post_compact hook payloads carry it filterable, since
    // "auto compacted" and "the user ran /compact" are different events even
    // when the mechanics are identical. A non-empty `prompt_override` (/compact
    // <name>) replaces the preset's compression prompt for THIS invocation only;
    // auto-compact and bare /compact never pass one.
    std::optional<std::size_t> Compact(std::string_view trigger,
                                       std::string_view prompt_override = {});
    void FlushInjectedMessages();
    // The while(true) body both RunTurn overloads share. Takes `result` by
    // reference so the two entry points can differ only in seeding. The wake
    // path (no seed) does its initial injection flush here; the seeded path
    // flushed BEFORE appending the seed, which is the chronological order.
    void RunTurnLoop(TurnResult& result, bool flush_first);
    bool Cancelled() const;
    // The pause probe. Same call-shape as Cancelled(): an injected predicate,
    // nullopt-safe. Checked at the loop top only.
    bool Paused() const;
    // Waits before retry `attempt`, polling cancellation. False means "give up"
    // -- cancelled during the wait -- so the caller stops retrying. The
    // server's own hint (a parsed Retry-After) outranks the computed backoff:
    // retrying a 429 sooner than it permitted converts one rate-limit event
    // into max_retries more.
    bool SleepForBackoff(int attempt,
                         std::optional<std::chrono::milliseconds> retry_after = std::nullopt);

    // Rebindable for /model across providers, hence the wrapper rather than a
    // plain reference member.
    std::reference_wrapper<llm::LlmProvider> provider_;
    tools::ToolExecutor& executor_;
    tools::ToolRegistry& registry_;
    session::HistoryStore& store_;
    AgentPreset preset_;
    // The project's instructions (AGENTS.md/CLAUDE.md), rendered. Constant for
    // the session's life -- loaded once at creation/resume -- so it never
    // disturbs the prompt-cache prefix mid-session.
    std::string project_context_;
    std::vector<std::string> mcp_servers_;
    tools::CancelTokenPtr cancel_token_;
    // See OpenToSteering(). Raised once the turn's user-visible content is in
    // the history (after the seed append, or after the wake turn's top flush);
    // lowered when the turn loop exits. The reader thread's SubmitTurn polls
    // it mid-turn, hence the atomic.
    std::atomic<bool> turn_open_to_steering_{false};
    LoopConfig config_;
    SessionContext session_;
    std::function<bool()> is_cancelled_;
    std::function<bool()> is_paused_;
    llm::StreamEventSink event_sink_;
    // The test/direct-API injection seam, and the registry presets resolve
    // their `compaction_strategy` name against at COMPACTION TIME (not
    // construction time -- a mid-session /agent switch must change the format
    // the way it already changes compression_prompt). Both may be null/empty:
    // an injected compactor wins outright (tests), and a null registry just
    // skips the preset-name step of the resolution inside Compact().
    Compactor compactor_;
    const CompactionStrategyRegistry* compaction_strategies_ = nullptr;

    std::vector<llm::Message> history_;
    // Compaction-checkpoint bookkeeping: where the persisted suffix of history_
    // begins in the store's seq space, and whether the head message is the
    // synthetic summary (which exists only in history_, never in the store).
    // Together they yield the boundary_seq a checkpoint is written with:
    // persisted messages covered by the next summary end just before
    // first_persisted_seq_ after the fold. Both are worker-thread-only state,
    // like everything else on the loop except the injection queue.
    std::optional<std::int64_t> first_persisted_seq_;
    bool head_is_summary_ = false;
    // Shared with the session's channels block (see SessionContext::injections).
    // The one loop member foreign threads touch, and only through the queue's
    // own mutex.
    std::shared_ptr<InjectionQueue> injections_;
    llm::TokenAccumulator tokens_;
    // Backoff jitter only. Loop-owned rather than shared, so two sessions
    // retrying the same failing backend do not draw the same delays.
    std::mt19937 jitter_{std::random_device{}()};

    // Parallel tool dispatch (ARCHITECTURE.md "Tool dispatch"). Width from
    // LoopConfig; width 1 never starts threads. Lives on the loop rather than
    // the Session so a loop-only test exercises the real dispatch path, and so
    // the pool cannot outlive the executor/registry it borrows -- the loop is
    // destroyed first (Session declares it before its borrowed state).
    tools::ToolPool pool_;

    // Set when a compaction attempt fails, cleared at each RunTurn: without it a
    // failing summarizer would be re-invoked on every tool cycle of the same turn,
    // resending half the context to the provider each time. The trigger stays armed
    // for the next turn, where the flag resets.
    bool compaction_failed_this_turn_ = false;

    // Warn-once latch for a window-relative trigger that has no window to resolve
    // against: MaybeCompact runs on every tool cycle, and the situation is one
    // actionable line per session, not noise per cycle.
    bool compaction_trigger_warned_ = false;

    // Same latch, same reasoning, for a preset whose `compaction_strategy`
    // name nothing registered: one actionable line per session, not one per
    // compaction attempt.
    bool compaction_strategy_warned_ = false;
    // Auto-compaction sanity guards (MaybeCompact). `auto_compacted_last_check_`:
    // the previous trigger check compacted, so a trigger firing again at the
    // very next check means the fold cannot get the context under the trigger
    // -- auto-compaction halts (`auto_compaction_halted_`) with a notice
    // instead of re-summarizing every tool round, until occupancy drops below
    // the trigger again. `compaction_floor_warned_`: the trigger sits at or
    // below the context no compaction can remove (system prompt, tools,
    // project context), noticed once.
    bool auto_compacted_last_check_ = false;
    bool auto_compaction_halted_ = false;
    bool compaction_floor_warned_ = false;

    // A compaction that did NOT fold (a guard stopped it): logged, sent to the
    // client as a skipped CompactionEvent, and recorded for scrollback.
    void EmitCompactionNotice(std::string_view trigger, std::string warning);
};

}  // namespace minimal::agent
