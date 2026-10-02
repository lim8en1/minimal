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
      pool_(config.max_parallel_tool_calls) {
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
            break;
        }

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

        // Parallel dispatch (ARCHITECTURE.md "Tool dispatch"), in two phases.
        //
        // Phase 1 -- GATE. Every call's permission decision resolves here,
        // sequentially on this worker, in assistant-block order. Prompts still
        // arrive one at a time (better UX than five simultaneous modals); a
        // grant an earlier call of the round acquires satisfies later ones; and
        // nothing below can park in the permission checker on a pool thread,
        // which is what keeps Session::Terminate's fixed order deadlock-free.
        // Prepare never throws: a gate refusal or a plugin throw becomes the
        // denial result Execute returns.
        struct PendingCall {
            std::size_t index;  // position in assistant-block order
            tools::PreparedInvocation prepared;
            tools::ToolContext ctx;
            bool to_pool = false;
        };
        std::vector<PendingCall> calls;
        calls.reserve(tool_uses.size());
        // "Deny and stop" (PreparedInvocation::stop_turn): once any call of the
        // round is denied that way, the operator is done with this turn -- the
        // later calls are answered without prompting, the earlier (already
        // allowed) ones without running, and no provider call follows. Nothing
        // has executed yet when the flag lands: gating precedes all execution.
        bool stop_requested = false;
        const auto stopped_by_user = [](const std::string& tool_use_id) {
            return llm::ToolResultBlock{
                tool_use_id, {llm::TextBlock{"not run: the user stopped the turn"}}, true};
        };
        for (std::size_t i = 0; i < tool_uses.size(); ++i) {
            const auto* use = tool_uses[i];
            // Aggregate-built around its context: ToolContext holds a Logger
            // reference, so it (and therefore PendingCall) is construct-in-place
            // only -- no default construction, no assignment.
            PendingCall call{i,
                             {},
                             [&] {
                                 tools::ToolContext ctx{session_.logger};
                                  ctx.session_id = session_.session_id;
                                  ctx.project_root = session_.project_root;
                                  // Map -> plain SkillAccess rows: Preload/Deny never
                                  // reach the tool (preloaded bodies are in the prompt;
                                  // denied names are absent = refused); Ask rides as
                                  // the flag the tool narrows its own mode with.
                                  for (const auto& [skill, mode] : preset_.allowed_skills) {
                                      ctx.allowed_skills.push_back(
                                          {skill, mode == SkillMode::Ask});
                                  }
                                  ctx.is_cancelled = is_cancelled_;
                                 ctx.cancel_token = cancel_token_;
                                 // Per invocation, from the Session-owned window: a
                                 // subagent sees only its own transcript, because the
                                 // window is bound to THIS session's id
                                 // (docs/EXTENSIBILITY.md §2.1).
                                 ctx.history = session_.history;
                                  if (session_.tool_output_sink) {
                                      const std::string tool_use_id = use->id;
                                      // A process's reads split anywhere -- also inside
                                      // a multi-byte character. The incomplete tail is
                                      // held back and prepended to the next chunk, so
                                      // the sink's UTF-8 scrub (session.cpp) does not
                                      // turn it into U+FFFD. Locked: stdout and stderr
                                      // readers may emit concurrently.
                                      struct Carry {
                                          std::mutex mu;
                                          std::string pending;
                                      };
                                      ctx.emit_output = [this, tool_use_id,
                                                         carry = std::make_shared<Carry>()](
                                                            std::string_view chunk) {
                                          std::string text;
                                          {
                                              std::lock_guard lock(carry->mu);
                                              text = std::move(carry->pending);
                                              carry->pending.clear();
                                              text.append(chunk);
                                              const std::size_t tail =
                                                  common::IncompleteUtf8Tail(text);
                                              carry->pending.assign(text, text.size() - tail,
                                                                    tail);
                                              text.resize(text.size() - tail);
                                          }
                                          if (!text.empty()) {
                                              session_.tool_output_sink(tool_use_id, text);
                                          }
                                      };
                                  }
                                  // Deferred-completion surface, copied verbatim from the session's
                                  // wiring (see SessionContext): a tool that schedules work beyond
                                  // its own invocation captures these, and their lifetime is the
                                  // session's shared channels block -- not this call, not this loop.
                                  ctx.schedule_injection = session_.injection_sink;
                                  ctx.session_lifetime = session_.session_lifetime;
                                  ctx.emit_task_event = session_.task_event_sink;
                                  ctx.prompt_user = session_.prompt_user;
                                  return ctx;
                              }(),
                              false};

            // Recorded at gate time, in block order: what ran is the question,
            // not what succeeded, and call order is the model's order -- not
            // the order executions happen to finish in.
            result.tool_names_invoked.push_back(use->name);

            // THE GLOBAL TOOL-INPUT CEILING, checked before anything else
            // consumes the input: no hook payload, no permission request, no
            // execution -- one error result the model adapts to, the same
            // shape as every other refusal. (The lifecycle events still fire
            // with the input, so a client panel shows WHAT was refused.)
            tools::PreparedInvocation prepared;
            prepared.tool_use_id = use->id;
            prepared.tool_name = use->name;
            bool denied_for_input = false;
            // A cancel landing during an earlier call's prompt must also stop
            // the remaining gates: otherwise each one raises a fresh prompt
            // (the epoch bump only released the waiter that was parked).
            if (!stop_requested && Cancelled()) stop_requested = true;
            if (stop_requested) {
                // Not prompted, not run: see stop_requested above. Takes the
                // same skip-Prepare path as an oversized input.
                prepared.denial = stopped_by_user(use->id);
                denied_for_input = true;
            } else if (use->name == kRecordSummaryToolName) {
                // Declared on every request for the summarizer's sake (see
                // BuildSystemAndTools); outside a compaction it records nothing.
                prepared.denial = llm::ToolResultBlock{
                    use->id,
                    {llm::TextBlock{"record_summary is only used when the harness asks for a "
                                    "context summary; nothing was recorded. Continue the task."}},
                    true};
                denied_for_input = true;
            } else if (config_.max_tool_input_bytes > 0) {
                const std::size_t input_bytes = use->input.dump().size();
                if (input_bytes > static_cast<std::size_t>(config_.max_tool_input_bytes)) {
                    prepared.denial = llm::ToolResultBlock{
                        use->id,
                        {llm::TextBlock{"tool input exceeds the configured max_input_bytes (" +
                                        std::to_string(config_.max_tool_input_bytes) +
                                        " bytes); send a smaller input"}},
                        true};
                    denied_for_input = true;
                }
            }
            if (!denied_for_input) {
                // Per-preset plugin config, resolved BEFORE Prepare: Disclose()
                // and grant checks run inside it, so the disclosure (Ask prompt,
                // grant keys) must already see the slice the tool's
                // config_namespace() claims in the active preset's tool_config.
                // Points into preset_, which is stable for the turn -- preset
                // swaps land on this worker between turns (Session::ApplyPreset),
                // never inside one. The Find is the same lookup PrepareGuarded
                // performs; AliasedTool forwards config_namespace().
                if (const auto tool = registry_.Find(use->name)) {
                    const std::string_view ns = tool->config_namespace();
                    if (!ns.empty()) {
                        if (auto it = preset_.tool_config.find(std::string(ns));
                            it != preset_.tool_config.end()) {
                            call.ctx.tool_config = &it.value();
                        }
                    }
                }
                prepared = executor_.Prepare(use->id, use->name, use->input, call.ctx);
                if (prepared.stop_turn) stop_requested = true;
            }
            // Unknown tools arrive here with no resolved tool; they execute as
            // their denial result and have no exclusivity to protect.
            call.to_pool = !denied_for_input && prepared.tool &&
                           prepared.tool->parallel_safe();
            call.prepared = std::move(prepared);
            calls.push_back(std::move(call));
        }

        // Calls gated BEFORE the stopping one were allowed but have not run:
        // stop means stop, so they are answered unrun too. Every tool_use still
        // gets its result, which keeps the transcript paired.
        if (stop_requested) {
            for (auto& call : calls) {
                if (call.prepared.denial) continue;
                call.prepared.denial = stopped_by_user(tool_uses[call.index]->id);
                call.to_pool = false;
            }
        }

        // Phase 2 -- RUN. The brackets below run on whichever thread executes
        // the call: both sinks are thread-safe (the lifecycle/output sinks end
        // in the session's EventQueue, already multi-producer; the observer
        // registry dispatches internally on its one ordered thread). Results
        // land by index, so the appended User message is in block order
        // regardless of completion order -- a deterministic transcript and a
        // stable prompt-cache prefix.
        std::vector<llm::ContentBlock> results;
        results.resize(tool_uses.size());
        // Wire-event failure must never take the tool result down: the sinks
        // end in the session's EventQueue, the transcript is the authoritative
        // record, and a throw here would otherwise unwind through a pool task.
        auto emit_lifecycle = [this](const ToolLifecycleEvent& event) {
            try {
                if (session_.tool_lifecycle_sink) session_.tool_lifecycle_sink(event);
            } catch (const std::exception& error) {
                session_.logger.warn("tool lifecycle event dropped: {}", error.what());
            } catch (...) {
                session_.logger.warn("tool lifecycle event dropped (unknown exception)");
            }
        };
        auto execute_one = [&](PendingCall& call) {
            const auto* use = tool_uses[call.index];
            ToolLifecycleEvent lifecycle;
            lifecycle.tool_use_id = use->id;
            lifecycle.tool_name = use->name;
            lifecycle.input = use->input;
            lifecycle.started = true;
            lifecycle.started_at_unix_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            emit_lifecycle(lifecycle);

            const auto tool_started = std::chrono::steady_clock::now();
            llm::ToolResultBlock tool_result;
            try {
                tool_result = executor_.Execute(call.prepared, call.ctx);
            } catch (const std::exception& error) {
                // Execute never throws by contract (tool_executor.hpp) -- but
                // this is the one seam whose failure mode is std::terminate
                // THROUGH a pool worker, and an unwind here would also race
                // the round's other submitted tasks against this stack frame.
                // An error result keeps every guarantee instead: the transcript
                // stays paired, the round appends, the model adapts.
                session_.logger.warn("tool {} threw past its executor guard: {}", use->name,
                                     error.what());
                tool_result = llm::ToolResultBlock{
                    use->id, {llm::TextBlock{"tool execution failed unexpectedly"}}, true};
            } catch (...) {
                session_.logger.warn("tool {} threw past its executor guard (unknown exception)",
                                     use->name);
                tool_result = llm::ToolResultBlock{
                    use->id, {llm::TextBlock{"tool execution failed unexpectedly"}}, true};
            }

            // THE GLOBAL TOOL-RESULT CEILING (see CapToolResult): applies
            // regardless of which tool produced this, including MCP tools and
            // any third-party plugin tool that caps nothing on its own.
            CapToolResult(tool_result, ToolResultByteCap());
            if (const std::size_t dropped =
                    CapToolDisplay(tool_result, config_.max_tool_display_bytes)) {
                session_.logger.warn(
                    "tool {}: display payload ({} bytes) exceeds tools.max_display_bytes ({}); "
                    "dropped, the client falls back to text",
                    use->name, dropped, config_.max_tool_display_bytes);
            }

            // Emitted even when Execute() reports an error, and before the
            // result is appended: a client that showed a tool as started must
            // always see it finish, or its running-tools panel accumulates
            // entries that never clear. The input rode the STARTED event; the
            // finish event carries the outcome, not another full copy of the
            // input -- for a Write with a 1 MB payload that was a second
            // megabyte into the (now byte-bounded) outbound queue per call,
            // for data every consumer already had.
            lifecycle.started = false;
            lifecycle.input = {};
            lifecycle.is_error = tool_result.is_error;
            lifecycle.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tool_started);
            emit_lifecycle(lifecycle);

            results[call.index] = std::move(tool_result);
        };

        // Parallel-safe calls go to the pool first, so the serial ones below
        // overlap as much of the batch as possible. Serial-only tools
        // (parallel_safe() == false) run here on the worker, one at a time, in
        // block order: their contract is "my invocations never overlap each
        // other", and the worker is the thread that guarantees it.
        //
        // RAII, not a bare call after both loops: pool tasks capture `&call`
        // into `calls`, which lives on THIS stack frame (ToolPool rule 3 --
        // the caller's stack must outlive Wait()). If anything between the
        // submit loop and the explicit Wait() below throws -- a serial
        // execute_one(), or Submit() itself hitting the pool's own
        // exceptions -- the stack would unwind past `calls` without ever
        // joining the pool, leaving its threads holding dangling references.
        // Wait() is documented idempotent, so the guard's destructor and the
        // explicit call below (kept for the ordinary, non-throwing path's
        // readability) cost nothing extra when both run.
        struct PoolWaitGuard {
            tools::ToolPool& pool;
            ~PoolWaitGuard() { pool.Wait(); }
        } wait_guard{pool_};

        for (auto& call : calls) {
            if (call.to_pool) {
                // `call` lives in `calls` on this stack until Wait() returns,
                // which is why a reference capture is sound (ToolPool rule 3).
                pool_.Submit([&call, &execute_one] { execute_one(call); });
            }
        }
        for (auto& call : calls) {
            if (!call.to_pool) execute_one(call);
        }
        pool_.Wait();

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
