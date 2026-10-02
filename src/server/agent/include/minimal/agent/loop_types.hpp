#pragma once

// The value types the agent loop and its components share: configuration
// (LoopConfig), the callbacks Session wires in (SessionContext), the events the
// loop emits, and a turn's result / resume seed. Split out of agent_loop.hpp so
// the loop's internal components can use them without including the loop.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <functional>

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_compactor.hpp"
#include "minimal/agent/injection_queue.hpp"
#include "minimal/agent/retry_policy.hpp"
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
        // Clamps max_tokens so the summarizer request fits the window.
        std::optional<std::int64_t> context_window_tokens = std::nullopt;
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
    //      name (typo, or the providing plugin is disabled) warns once (in
    //      Compact(), which owns the per-loop once-flag) and
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

    // The plugin prompt (ToolContext::prompt_user), forwarded from the
    // session's PromptBroker wiring. Null when the session has no prompt
    // broker (see SessionParams::prompt_broker) -- a prompting tool then
    // reports "unavailable" instead of parking a worker nobody can answer.
    // Unlike the handles above this BLOCKS for a human, so it must only run
    // where tool execution already allows blocking.
    std::function<tools::PromptResult(const tools::PromptRequest&)> prompt_user;

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

}  // namespace minimal::agent
