#include "minimal/agent/agent_loop.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

#include "minimal/agent/compaction_boundary.hpp"
#include "minimal/agent/compaction_trigger.hpp"
#include "minimal/agent/structured_compaction.hpp"
#include "minimal/agent/transcript_repair.hpp"
#include "minimal/agent/compaction_controller.hpp"
#include "minimal/agent/request_builder.hpp"
#include "minimal/agent/tool_round.hpp"
#include "minimal/agent/working_history.hpp"
#include "minimal/agent/retry_policy.hpp"
#include "tool_result_caps.hpp"
#include "minimal/common/utf8.hpp"

namespace minimal::agent {
namespace {


}  // namespace

AgentLoop::AgentLoop(llm::LlmProvider& provider, tools::ToolExecutor& executor,
                      tools::ToolRegistry& registry, session::HistoryStore& store, AgentPreset preset,
                      LoopConfig config, SessionContext session,
                      std::function<bool()> is_cancelled, std::function<bool()> is_paused,
                      llm::StreamEventSink event_sink,
                      Compactor compactor, ResumeSeed seed, std::string project_context,
                      const CompactionStrategyRegistry* compaction_strategies)
    : provider_(provider),
      executor_(executor),
      registry_(registry),
      store_(store),
      preset_(std::move(preset)),
      config_(config),
      session_(std::move(session)),
      is_cancelled_(std::move(is_cancelled)),
      is_paused_(std::move(is_paused)),
      event_sink_(std::move(event_sink)),
      working_(store, std::move(seed.history), seed.first_persisted_seq, seed.head_is_summary),
      request_(preset_, std::move(project_context), registry_, config_, working_.messages(),
               working_.tokens(), is_cancelled_, session_.session_id),
      compaction_(preset_, config_, session_, store, working_, request_, provider_,
                  std::move(compactor), compaction_strategies, is_cancelled_),
      retry_(config.retry_base_delay, config.retry_max_delay, is_cancelled_),
      tool_round_(executor, registry, preset_, config_, session_, request_, is_cancelled_,
                  config.max_parallel_tool_calls) {
    // The injection queue is shared with the session's channels block when
    // there is one (the producers are foreign threads and must see the same
    // queue the session's wake predicate and Terminate do). A plain test loop
    // gets a private one, which nothing else can observe anyway.
    injections_ =
        session_.injections ? session_.injections : std::make_shared<InjectionQueue>();
    if (!working_.messages().empty()) {
        // A resumed context has no backend usage behind it yet, so give the
        // compaction trigger an estimate to work with. Deliberately the same
        // bytes/4 roughness the compactor's fallback uses -- there is no
        // tokenizer in this codebase, and the first real response replaces the
        // estimate with ground truth anyway. Covers the whole request context
        // (system prompt, project context, tools array), not just the history:
        // those ride every request and backend ground truth counts them, so
        // excluding them under-triggered compaction on exactly the sessions
        // this seed exists for.
        working_.tokens().SeedOccupancy(EstimateContextTokens());
    }
    // A checkpoint resume reproduces the live post-fold working set, which
    // had the kept messages' thinking dropped (see Compact()).
    if (working_.head_is_summary() && working_.first_persisted_seq().has_value() && seed.kept_through_seq >= 0) {
        const std::int64_t kept = seed.kept_through_seq - *working_.first_persisted_seq() + 1;
        if (kept > 0) {
            compaction_.DropStaleThinking(1, std::min(working_.messages().size(), static_cast<std::size_t>(kept) + 1));
        }
    }
}

bool AgentLoop::Cancelled() const { return is_cancelled_ && is_cancelled_(); }

bool AgentLoop::Paused() const { return is_paused_ && is_paused_(); }

bool AgentLoop::HistoryEndsWithUnansweredUserText() const {
    return working_.EndsWithUnansweredUserText();
}

session::MessageAttribution AgentLoop::AssistantAttribution() const {
    session::MessageAttribution attribution;
    attribution.agent_preset = preset_.name;
    // str() of an unset ref is "/", which would persist as a bogus model
    // rather than as unknown.
    if (!preset_.model_ref.provider.empty() && !preset_.model_ref.model.empty()) {
        attribution.model = preset_.model_ref.str();
    }
    return attribution;
}

void AgentLoop::AppendAndPersist(llm::Message message, std::optional<int> output_tokens) {
    // Assistant rows carry who produced them: preset_ is the EFFECTIVE preset
    // for this turn (Session swaps in a per-turn borrow before RunTurn and
    // restores it after), so a one-turn override is recorded as exactly that.
    const session::MessageAttribution attribution =
        message.role == llm::Role::Assistant ? AssistantAttribution()
                                             : session::MessageAttribution{};
    working_.Append(std::move(message), output_tokens, attribution);
}

// Unrelated to Session::QueueInjectedMessage despite the similar shape: that
// one guards on terminated_ and wakes the session's own worker via turn_cv_,
// neither of which AgentLoop has -- this is a bare push, used directly only
// by tests that construct a bare AgentLoop with no Session wrapper.
bool AgentLoop::PushInjectedMessage(llm::Message message) {
    return injections_->Push(std::move(message));
}

void AgentLoop::FlushInjectedMessages() {
    // DrainAll runs the sink outside the queue's mutex; AppendAndPersist
    // touches the store and must not hold it.
    injections_->DrainAll([this](llm::Message message) {
        // REAL-TIME STEERING VETO. A user-text injection is a human prompt
        // arriving mid-turn, so it crosses the same seam a turn-pop prompt
        // does: core.user_prompt_submit, on the worker, where the hook's
        // subprocess budget is allowed to block. Without this, steering would
        // be the one input path that silently bypasses a guard hook -- and
        // "hooks can only narrow" would have a hole shaped exactly like the
        // feature. A veto drops the message before it is persisted; the gate
        // surfaces the reason on the wire itself.
        if (IsUserTextMessage(message) && session_.steer_gate && !session_.steer_gate(message)) {
            return;
        }
        AppendAndPersist(std::move(message));
    });
}

// Request building lives in RequestBuilder; these keep the loop's own call sites
// (and the public BuildChatRequest / EstimateContextTokens) unchanged.
AgentLoop::SystemAndTools AgentLoop::BuildSystemAndTools() const {
    return request_.BuildSystemAndTools();
}
llm::ChatRequest AgentLoop::BuildChatRequest() const { return request_.BuildChatRequest(); }
std::int64_t AgentLoop::EstimateContextTokens() const { return request_.EstimateContextTokens(); }
std::int64_t AgentLoop::EstimateContextTokens(const std::optional<std::string>& system,
                                              const std::vector<llm::ToolDefinition>& tools) const {
    return request_.EstimateContextTokens(system, tools);
}
std::int64_t AgentLoop::ToolResultByteCap() const { return request_.ToolResultByteCap(); }

// Compaction lives in CompactionController; these keep the loop's call sites and
// public CompactNow unchanged.
void AgentLoop::MaybeCompact() { compaction_.MaybeCompact(); }
std::optional<std::size_t> AgentLoop::CompactNow(std::string_view prompt_override) {
    return compaction_.CompactNow(prompt_override);
}
std::optional<std::size_t> AgentLoop::Compact(std::string_view trigger,
                                              std::string_view prompt_override) {
    return compaction_.Compact(trigger, prompt_override);
}

void AgentLoop::ClearHistory() {
    working_.Clear();
    // The queue is concurrency-safe by construction (its own mutex), and the
    // two sides are genuinely concurrent: /clear runs on THIS session's worker,
    // while QueueInjectedMessage runs on a NON-BLOCKING SUBAGENT's worker from
    // its completion hook -- or on a background task's watcher. A background
    // child finishing while the user types /clear is a data race on a
    // vector<Message> -- the kind that corrupts rather than crashes.
    injections_->Clear();
    // /clear drops the VIEW, and the checkpoint IS a view: leaving it would
    // make a resume load [summary + tail] of a conversation the user wiped
    // from context, when clear's contract is that only the transcript survives
    // -- so resume goes back to the full-transcript load. Best-effort, like
    // the save: a failed delete degrades to a resume that restores the last
    // summary, not to a lost transcript.
    try {
        store_.ClearCompactionCheckpoint();
    } catch (const std::exception& error) {
        session_.logger.warn("session {}: failed to clear compaction checkpoint: {}",
                             session_.session_id, error.what());
    }
}

// What a working-set replacement owes afterwards: the token estimate described
// the old contents, and -- when a tail was DISCARDED (rewind, reseed) -- queued
// completions answered turns that no longer happened. A redo discards nothing,
// so it keeps them.
void AgentLoop::AfterWorkingSetReplaced(bool discarded_tail) {
    working_.tokens() = llm::TokenAccumulator{};
    if (!working_.messages().empty()) {
        working_.tokens().SeedOccupancy(EstimateContextTokens());
    }
    if (discarded_tail) {
        // Same queue ClearHistory clears (its own mutex, so no lock needed here):
        // a background subagent's worker can be queueing a completion while the
        // user rewinds.
        injections_->Clear();
    }
}

void AgentLoop::TruncateAfter(std::int64_t target_seq) {
    switch (working_.TruncateAfter(target_seq)) {
        case WorkingHistory::TruncateResult::kNothingToTruncate:
            return;
        case WorkingHistory::TruncateResult::kUnmapped:
            // Defensive -- the Session validates the target against the store
            // before calling, and every path that fills the working set also
            // establishes the mapping -- but silent corruption would be worse
            // than an early return.
            session_.logger.warn("session {}: rewind ignored: no persisted portion to map onto",
                                 session_.session_id);
            return;
        case WorkingHistory::TruncateResult::kTruncated:
            break;
    }
    AfterWorkingSetReplaced(/*discarded_tail=*/true);
    session_.logger.info(
        "session {}: rewound to seq {}; {} messages remain in the working set",
        session_.session_id, target_seq, working_.messages().size());
}

void AgentLoop::ReseedFromStore(std::vector<llm::Message> rows, std::int64_t first_seq) {
    working_.Reseed(std::move(rows), first_seq);
    AfterWorkingSetReplaced(/*discarded_tail=*/true);
    session_.logger.info(
        "session {}: rewound under the compaction boundary to seq {}; working set rebuilt "
        "with {} messages",
        session_.session_id, first_seq, working_.messages().size());
}

void AgentLoop::RestorePersisted(std::vector<llm::Message> rows, std::int64_t first_seq) {
    if (!working_.Restore(std::move(rows), first_seq)) return;
    AfterWorkingSetReplaced(/*discarded_tail=*/false);
    session_.logger.info("session {}: redo restored {} messages from seq {}",
                         session_.session_id, working_.messages().size(), first_seq);
}

namespace {
// Closes steering eligibility on EVERY exit from a turn, the throwing ones
// included: after an LlmError the worker is still "running a turn" for the
// post-turn bookkeeping, and a submission steered into that window would be
// answered by nobody.
struct SteeringCloser {
    std::atomic<bool>& flag;
    ~SteeringCloser() { flag.store(false, std::memory_order_release); }
};
}  // namespace

TurnResult AgentLoop::RunTurn(llm::Message seed_message) {
    TurnResult result;
    SteeringCloser steering_closer{turn_open_to_steering_};
    compaction_.BeginTurn();
    // A stale open flag from a turn that exited via exception (RunTurnLoop's
    // rethrows skip the trailing lower) must not outlive it: steering
    // eligibility is re-derived for THIS turn.
    turn_open_to_steering_.store(false, std::memory_order_release);
    // TURN START, before the seed and the first BuildChatRequest: anything
    // queued out-of-band while the session was idle -- a non-blocking
    // subagent's completion, a background task's result -- enters the history
    // here. Without this flush an injection that landed on an idle session
    // would MISS the very turn that should have carried it: the loop would
    // build its request, call the provider, and only then append the message.
    // The ordering is also chronological -- the injection happened before the
    // user typed the seed.
    FlushInjectedMessages();
    AppendAndPersist(std::move(seed_message));
    // THE STEERING BOUNDARY. Only past this point may Session route a
    // mid-turn submission onto the injection queue: before the seed is in the
    // history, the turn-start flush above would splice the submission in
    // FRONT of the seed it was submitted after -- reversing the user's
    // submission order on the durable transcript. A submission landing in
    // that window queues on the work queue instead (Session's SubmitTurn
    // polls OpenToSteering), and runs its own turn.
    turn_open_to_steering_.store(true, std::memory_order_release);
    RunTurnLoop(result, /*flush_first=*/false);
    turn_open_to_steering_.store(false, std::memory_order_release);
    return result;
}

TurnResult AgentLoop::RunTurn() {
    TurnResult result;
    SteeringCloser steering_closer{turn_open_to_steering_};
    compaction_.BeginTurn();
    turn_open_to_steering_.store(false, std::memory_order_release);
    // The wake path: no seed of its own -- the injected messages ARE the user
    // turn, flushed at the top of the loop body. Steering opens only after
    // that flush (RunTurnLoop), so a submission can never precede the content
    // this turn exists to answer.
    RunTurnLoop(result, /*flush_first=*/true);
    turn_open_to_steering_.store(false, std::memory_order_release);
    return result;
}

// One provider request with the loop's retry policy: a context-overflow refusal
// compacts and resends once (without spending a retry); other retryable errors back
// off and retry up to max_retries; anything else rethrows. nullopt means the turn
// must stop WITHOUT a response -- today only a cancel landing inside the backoff
// sleep, which is reported as Cancelled rather than as a model failure.
std::optional<llm::ChatResponse> AgentLoop::StreamWithRetry(llm::ChatRequest& chat_request,
                                                            TurnResult& result) {
    llm::ChatResponse response;
    bool got_response = false;
    bool cancelled_in_backoff = false;
    bool overflow_recovered = false;
    for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
        try {
            response = provider_.get().StreamComplete(chat_request, event_sink_);
            got_response = true;
            break;
        } catch (const llm::LlmError& error) {
            // The request did not fit the window (the estimate undercounted,
            // or the window is smaller than declared): compact once and
            // resend, instead of failing this turn and every later one.
            if (!overflow_recovered && config_.auto_compact && RetryPolicy::IsContextOverflow(error)) {
                overflow_recovered = true;
                session_.logger.warn(
                    "session {}: provider rejected the request as too large ({}); "
                    "compacting and retrying once",
                    session_.session_id, error.what());
                if (Compact("overflow").has_value()) {
                    chat_request = BuildChatRequest();
                    --attempt;  // the recovery does not spend a retry
                    continue;
                }
            }
            // Retry policy lives here, not in the provider: only the loop
            // knows whether another attempt is still wanted.
            if (!error.is_retryable() || attempt == config_.max_retries) {
                throw;
            }
            // Backoff, not a bare retry. Without it the four attempts land
            // inside a few milliseconds, which turns a 429 into four 429s
            // and is worse for the backend than not retrying at all.
            if (!retry_.SleepForBackoff(attempt, error.retry_after())) {
                // CancelTurn landed inside the backoff sleep. That is a
                // cancellation, not a mysterious failure: report it as the
                // Cancelled it is rather than letting the turn fall out with
                // StopReason::Unknown -- the headless exit code derives from
                // this value, and exit 3 ("model error") for a user cancel
                // is simply the wrong answer.
                cancelled_in_backoff = true;
                break;
            }
        }
    }
    if (!got_response) {
        if (cancelled_in_backoff) result.stop_reason = llm::StopReason::Cancelled;
        return std::nullopt;
    }
    return response;
}

void AgentLoop::RunTurnLoop(TurnResult& result, bool flush_first) {
    if (flush_first) {
        FlushInjectedMessages();
        turn_open_to_steering_.store(true, std::memory_order_release);
    }
    int turns = 0;
    while (true) {
        if (Cancelled()) {
            result.stop_reason = llm::StopReason::Cancelled;
            break;
        }

        // THE PAUSE CHECK -- the only one in the loop. The tool round in
        // flight when the pause lands always runs to completion and persists
        // its real results (the previous iteration's Phase 2 finished before
        // control returned here); this break therefore lands exactly on the
        // tool-call-return boundary, and the turn ends with the transcript
        // ending in a paired [tool_use, tool_result]. No further provider
        // call happens, so the model sees those results only when the session
        // resumes -- a steering message or /resume's seedless continuation.
        // Paused, not Cancelled: nothing in flight was harmed, and every
        // consumer of the stop reason (headless exit code, core.stop hook, a
        // subagent's parent) can tell "suspended, will continue" from
        // "halted".
        if (Paused()) {
            result.stop_reason = llm::StopReason::Paused;
            break;
        }

        if (config_.max_turns > 0 && turns >= config_.max_turns) {
            result.hit_max_turns = true;
            break;
        }
        ++turns;

        MaybeCompact();

        // Built ONCE per tool round, not per attempt: the request is identical
        // across retries (nothing this loop does mutates preset or registry),
        // and rebuilding it re-serialized the whole history on every retry.
        llm::ChatRequest chat_request = BuildChatRequest();
        // Set before the call, not from the response: a turn that fails or is
        // cancelled before any response still reports the model it was aimed
        // at, and TurnSummary::model is populated on every exit path.
        result.model = chat_request.model;
        std::optional<llm::ChatResponse> streamed = StreamWithRetry(chat_request, result);
        if (!streamed) break;
        llm::ChatResponse response = std::move(*streamed);

        ++result.provider_calls;
        result.last_usage = response.usage;
        result.usage_estimated = config_.estimate_zero_usage && !llm::ReportedUsage(response.usage);
        // Zero-usage turns record nothing here (RecordTurn of an all-zero
        // Usage would only add zeros to the cumulative totals); the
        // occupancy seed below replaces last_turn_ once the assistant
        // message is actually in working_.messages() and the estimate can cover it.
        if (!result.usage_estimated) {
            working_.tokens().RecordTurn(response.usage);
            // Turn-cost accounting sums ACROSS rounds -- each round is a
            // separately-billed request, unlike context occupancy where
            // summing would double-count the history (token_accounting.hpp).
            // Estimated rounds contribute nothing; they only clear the
            // fully-reported flag below.
            result.turn_usage.input_tokens += response.usage.input_tokens;
            result.turn_usage.output_tokens += response.usage.output_tokens;
            result.turn_usage.cache_read_input_tokens +=
                response.usage.cache_read_input_tokens;
            result.turn_usage.cache_creation_input_tokens +=
                response.usage.cache_creation_input_tokens;
        } else {
            result.turn_usage_fully_reported = false;
        }
        // Mid-turn usage signal: fire per round, not per turn, so a live cost
        // readout moves while the turn (and any subagent it spawned) is still
        // working. Estimated rounds fire too, flagged -- the consumer keeps
        // the round's existence honest without pricing fabricated tokens.
        if (config_.on_provider_round) {
            RoundUsage round;
            round.model = result.model;
            round.usage = response.usage;
            round.usage_estimated = result.usage_estimated;
            config_.on_provider_round(std::move(round));
        }
        result.stop_reason = response.stop_reason;

        // MOVED, not copied. `response` is loop-local and dead after this point
        // -- the next iteration reassigns it -- and content can be the whole
        // assistant turn, so this was a full duplicate of every block and string
        // on the hottest path there is. The tool_uses pointers below stay valid
        // across the later move into working_.messages(): a vector move relocates the
        // heap buffer's ownership, not the elements.
        llm::Message assistant{llm::Role::Assistant, std::move(response.content)};
        result.final_text = llm::TextOf(assistant);
        const auto tool_uses = ToolUsesOf(assistant);
        // THE ROUND IS ONE UNIT. A tool round's assistant message is NOT
        // appended here: it is held until every call is answered and then
        // persisted TOGETHER with the results (AppendRound, one transaction),
        // so working_.messages() and the store can never hold a tool_use without its
        // results -- not across a cancel, a throw, a store fault or a crash --
        // and nothing (a steer, a rewind, a resume) can ever land between them.
        // There is nothing to repair afterwards because nothing is ever torn.
        const bool tool_round =
            response.stop_reason == llm::StopReason::ToolUse && !tool_uses.empty();
        // The only per-message token count that exists: this response's output.
        const std::optional<int> assistant_output_tokens = response.usage.output_tokens;
        if (!tool_round) {
            // The turn ends on this message, so tool_use blocks it carries (a
            // cancel or MaxTokens cut mid-stream, a refusal) are calls that
            // never ran: dropped rather than persisted as an unanswerable
            // orphan. An assistant message left empty by that is not stored.
            std::erase_if(assistant.content, [](const llm::ContentBlock& block) {
                return std::holds_alternative<llm::ToolUseBlock>(block);
            });
            if (!assistant.content.empty()) {
                AppendAndPersist(std::move(assistant), assistant_output_tokens);
            }
        }

        if (result.usage_estimated) {
            // Zero-usage proxy gap: the request is fully assembled now
            // (working_.messages() plus this round's assistant message), so the same
            // bytes/4 estimate resume and the compactor use describes the
            // context honestly enough to arm the trigger. SeedOccupancy
            // deliberately touches no cumulative totals, and the estimate is
            // mirrored into last_usage so the status bar renders -- the wire
            // flag and cost-tracker's unpriced rule keep it honest.
            //
            // Reuses chat_request.system/chat_request.tools -- BuildChatRequest
            // (a few lines above, this same tool round) already built them --
            // instead of the zero-arg overload, which would call
            // BuildSystemAndTools() a second time: a redundant
            // DefinitionsFor/DefinitionsForGroups materialization and sort
            // on every zero-usage round, which is every round against any
            // OpenAI-compatible proxy that does not report usage.
            std::int64_t estimate =
                EstimateContextTokens(chat_request.system, chat_request.tools);
            // A tool round's assistant message is not in working_.messages() yet.
            if (tool_round) estimate += estimate_tokens(std::span(&assistant, 1));
            working_.tokens().SeedOccupancy(estimate);
            result.last_usage.input_tokens =
                static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(estimate, 0),
                                                        std::numeric_limits<int>::max()));
        }

        // Anything but a tool round ends the turn. MaxTokens in particular is
        // reported as itself rather than collapsed into EndTurn -- otherwise
        // output is truncated mid-sentence and the turn looks successful.
        if (!tool_round) break;

        // Gate every call, then run them (parallel-safe ones on the pool). The commit
        // below stays here: it needs the held assistant message and the store.
        ToolRound::Outcome round = tool_round_.Run(tool_uses, result.tool_names_invoked);
        std::vector<llm::ContentBlock>& results = round.results;
        const bool stop_requested = round.stop_requested;

        // The round commits as one unit: the held assistant message and every
        // result, in a single store transaction (see tool_round above). A store
        // failure throws with NOTHING written and working_.messages() untouched, so the
        // session survives to its next turn.
        {
            llm::Message results_message{llm::Role::User, std::move(results)};
            store_.AppendRound(assistant, assistant_output_tokens, AssistantAttribution(),
                               results_message);
            if (!working_.first_persisted_seq().has_value()) {
                // The pair is two rows; the first of them opens the suffix.
                working_.first_persisted_seq() = store_.last_appended_seq() - 1;
            }
            working_.messages().push_back(std::move(assistant));
            working_.messages().push_back(std::move(results_message));
        }

        // "Deny and stop": the round is answered and persisted; no provider
        // call follows. Cancelled is what the operator did -- halted the turn
        // -- and every consumer already reads it that way (the headless exit
        // code, the stop hook, a subagent's parent).
        if (stop_requested) {
            result.stop_reason = llm::StopReason::Cancelled;
            break;
        }

        // Turn boundary: safe to splice in anything a background subagent
        // finished while this turn was running.
        FlushInjectedMessages();
    }

    FlushInjectedMessages();
}

}  // namespace minimal::agent
