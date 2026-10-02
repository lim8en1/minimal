#pragma once

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
#include "minimal/agent/loop_types.hpp"
#include "minimal/agent/compaction_controller.hpp"
#include "minimal/agent/request_builder.hpp"
#include "minimal/agent/tool_round.hpp"
#include "minimal/agent/working_history.hpp"

namespace minimal::agent {

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

    const std::vector<llm::Message>& history() const { return working_.messages(); }

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
    std::optional<std::int64_t> CheckpointBoundary() const { return working_.CheckpointBoundary(); }

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

    // The allowed-AND-active tool groups, resolved by the session through the
    // tool sources at construction and at every /agent switch (a preset
    // change can re-scope them). A dead member's exclusion is exactly this
    // list: the model never sees tools for a group that is not in it.
    // WORKER THREAD ONLY, like SetPreset.
    void SetToolGroups(std::vector<std::string> groups) { request_.SetToolGroups(std::move(groups)); }

    // The current turn's cancellation token, minted by the session when it
    // pops the turn. Handed to every ToolContext so blocking tools can wait
    // on its cv instead of polling. May be null between turns (tools cannot
    // observe that: they only run inside a turn). WORKER THREAD ONLY.
    void SetCancelToken(tools::CancelTokenPtr token) { tool_round_.SetCancelToken(std::move(token)); }

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
    using SystemAndTools = RequestBuilder::SystemAndTools;
    SystemAndTools BuildSystemAndTools() const;


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
    // Who produced an assistant row: the EFFECTIVE preset and model for this turn.
    session::MessageAttribution AssistantAttribution() const;
    void MaybeCompact();
    // The byte ceiling for one tool result: the configured cap, tightened to
    // what a quarter of the model's window can hold (about 4 bytes a token), so
    // one result can never fill the context on its own. 0 = no ceiling.
    std::int64_t ToolResultByteCap() const;
    // The shared body: every guard except the occupancy trigger, which is what
    // MaybeCompact adds and CompactNow deliberately skips. `trigger` names who
    // asked -- the pre/post_compact hook payloads carry it filterable, since
    // "auto compacted" and "the user ran /compact" are different events even
    // when the mechanics are identical. A non-empty `prompt_override` (/compact
    // <name>) replaces the preset's compression prompt for THIS invocation only;
    // auto-compact and bare /compact never pass one.
    std::optional<std::size_t> Compact(std::string_view trigger,
                                       std::string_view prompt_override = {});
    // After a rewind / reseed / redo replaced the working set: re-estimate tokens
    // and, if a tail was discarded, drop queued completions that answered turns
    // that no longer happened.
    void AfterWorkingSetReplaced(bool discarded_tail);
    void FlushInjectedMessages();
    // The while(true) body both RunTurn overloads share. Takes `result` by
    // reference so the two entry points can differ only in seeding. The wake
    // path (no seed) does its initial injection flush here; the seeded path
    // flushed BEFORE appending the seed, which is the chronological order.
    void RunTurnLoop(TurnResult& result, bool flush_first);
    // One provider request with retry, backoff and context-overflow recovery.
    std::optional<llm::ChatResponse> StreamWithRetry(llm::ChatRequest& chat_request,
                                                     TurnResult& result);
    bool Cancelled() const;
    // The pause probe. Same call-shape as Cancelled(): an injected predicate,
    // nullopt-safe. Checked at the loop top only.
    bool Paused() const;
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

    // The in-memory conversation, its seq mapping into the store, and the token
    // accumulator -- see WorkingHistory. Declared BEFORE request_, which holds
    // references to its messages and tokens.
    WorkingHistory working_;
    // Shared with the session's channels block (see SessionContext::injections).
    // The one loop member foreign threads touch, and only through the queue's
    // own mutex.
    std::shared_ptr<InjectionQueue> injections_;
    RequestBuilder request_;
    CompactionController compaction_;
    RetryPolicy retry_;

    // Gate + run of each tool round (parallel dispatch; ARCHITECTURE.md "Tool
    // dispatch"). Owns the ToolPool.
    ToolRound tool_round_;


};

}  // namespace minimal::agent
