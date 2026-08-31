#include "minimal/agent/agent_loop.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <variant>

#include "minimal/agent/compaction_boundary.hpp"
#include "minimal/agent/compaction_trigger.hpp"

namespace minimal::agent {
namespace {

// Concatenates every TextBlock in an assistant message. This is what a subagent
// hands back to its parent.
std::string TextOf(const llm::Message& message) {
    std::string out;
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block)) {
            out += text->text;
        }
    }
    return out;
}

std::vector<const llm::ToolUseBlock*> ToolUsesOf(const llm::Message& message) {
    std::vector<const llm::ToolUseBlock*> out;
    for (const auto& block : message.content) {
        if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) {
            out.push_back(use);
        }
    }
    return out;
}

}  // namespace

AgentLoop::AgentLoop(llm::LlmProvider& provider, tools::ToolExecutor& executor,
                      tools::ToolRegistry& registry, HistoryStore& store, AgentPreset preset,
                      LoopConfig config, SessionContext session,
                      std::function<bool()> is_cancelled, llm::StreamEventSink event_sink,
                      Compactor compactor, std::vector<llm::Message> seeded_history,
                      std::optional<std::int64_t> seed_first_persisted_seq,
                      bool seed_head_is_summary, std::string project_context)
    : provider_(provider),
      executor_(executor),
      registry_(registry),
      store_(store),
      preset_(std::move(preset)),
      project_context_(std::move(project_context)),
      config_(config),
      session_(std::move(session)),
      is_cancelled_(std::move(is_cancelled)),
      event_sink_(std::move(event_sink)),
      compactor_(std::move(compactor)),
      history_(std::move(seeded_history)),
      pool_(config.max_parallel_tool_calls),
      first_persisted_seq_(seed_first_persisted_seq),
      head_is_summary_(seed_head_is_summary) {
    // The injection queue is shared with the session's channels block when
    // there is one (the producers are foreign threads and must see the same
    // queue the session's wake predicate and Terminate do). A plain test loop
    // gets a private one, which nothing else can observe anyway.
    injections_ =
        session_.injections ? session_.injections : std::make_shared<InjectionQueue>();
    if (!history_.empty()) {
        // A resumed context has no backend usage behind it yet, so give the
        // compaction trigger an estimate to work with. Deliberately the same
        // bytes/4 roughness the compactor's fallback uses -- there is no
        // tokenizer in this codebase, and the first real response replaces the
        // estimate with ground truth anyway.
        tokens_.SeedOccupancy(estimate_tokens(history_));
    }
}

bool AgentLoop::Cancelled() const { return is_cancelled_ && is_cancelled_(); }

bool AgentLoop::SleepForBackoff(int attempt) {
    if (config_.retry_base_delay.count() <= 0) return !Cancelled();

    // Exponential with EQUAL jitter: half the ceiling fixed, half random.
    // Jitter matters even for one local user -- several sessions that failed
    // against the same backend would otherwise retry in lockstep forever -- but
    // full jitter (drawing from [0, ceiling]) can legally return zero, which
    // makes "the retry waited" unassertable in a single run and leaves a real
    // 429 answered immediately every so often. Half the ceiling is the floor.
    const auto ceiling = std::min(config_.retry_base_delay * (1 << attempt), config_.retry_max_delay);
    const std::int64_t half = ceiling.count() / 2;
    std::uniform_int_distribution<std::int64_t> pick(0, ceiling.count() - half);
    const std::chrono::milliseconds delay{half + pick(jitter_)};

    // Slept in slices, polling cancellation. Sleeping the whole delay would
    // make CancelTurn wait out a backoff the user just asked to abandon --
    // the same argument that put a cancel-polling sink in the compactor.
    constexpr std::chrono::milliseconds kSlice{25};
    for (std::chrono::milliseconds slept{0}; slept < delay; slept += kSlice) {
        if (Cancelled()) return false;
        std::this_thread::sleep_for(std::min(kSlice, delay - slept));
    }
    return !Cancelled();
}

void AgentLoop::AppendAndPersist(llm::Message message, std::optional<int> output_tokens) {
    store_.Append(message, output_tokens);
    // The persisted suffix gains its first member. Appends land at the END of
    // both history_ and the store, so this is the only place the boundary can
    // move forward.
    if (!first_persisted_seq_.has_value()) {
        first_persisted_seq_ = store_.last_appended_seq();
    }
    history_.push_back(std::move(message));
}

bool AgentLoop::QueueInjectedMessage(llm::Message message) {
    return injections_->Push(std::move(message));
}

void AgentLoop::FlushInjectedMessages() {
    // DrainAll runs the sink outside the queue's mutex; AppendAndPersist
    // touches the store and must not hold it.
    injections_->DrainAll([this](llm::Message message) { AppendAndPersist(std::move(message)); });
}

llm::ChatRequest AgentLoop::BuildChatRequest() const {
    llm::ChatRequest request;
    if (!preset_.system_prompt.empty()) request.system = preset_.system_prompt;
    // Project context appends AFTER the preset body: both are constant within
    // the session, so the request's prefix -- which the provider's prompt cache
    // keys on -- is stable either way, but appending keeps the preset's own
    // text at the very front across /agent switches.
    if (!project_context_.empty()) {
        if (request.system) *request.system += "\n\n";
        else request.system = std::string();
        *request.system += project_context_;
    }
    request.messages = history_;
    request.model = preset_.provider.model_name;
    request.parameters = preset_.parameters;
    request.max_tokens = preset_.max_tokens;
    request.stream = true;

    std::vector<std::string> allowed = preset_.allowed_tools;
    if (preset_.is_subagent) {
        // Hard rule, not preset config: a subagent never gets the spawn tool, so
        // a misconfigured preset cannot reopen recursion. Subagents are flat,
        // one level deep.
        allowed.erase(std::remove(allowed.begin(), allowed.end(), kSpawnSubagentToolName),
                      allowed.end());
    }

    request.tools = registry_.DefinitionsFor(allowed);
    // Live MCP tools for the allowed-and-alive servers, merged into the same
    // sorted order -- this request is rebuilt every turn, so the merge must
    // preserve the deterministic prefix the prompt cache keys on.
    {
        std::vector<llm::ToolDefinition> mcp_defs =
            registry_.DefinitionsForMcpServers(mcp_servers_);
        request.tools.insert(request.tools.end(), mcp_defs.begin(), mcp_defs.end());
        std::sort(request.tools.begin(), request.tools.end(),
                  [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                      return a.name < b.name;
                  });
    }
    if (preset_.is_subagent) {
        // DefinitionsFor("*") would have re-admitted it.
        request.tools.erase(std::remove_if(request.tools.begin(), request.tools.end(),
                                            [](const llm::ToolDefinition& def) {
                                                return def.name == kSpawnSubagentToolName;
                                            }),
                             request.tools.end());
    }
    if (preset_.allowed_skills.empty()) {
        // No skills in this preset: the tool would only ever return errors. One
        // post-erase suffices here (unlike the subagent case above) because it
        // covers both an explicit allowed_tools list and "*".
        request.tools.erase(std::remove_if(request.tools.begin(), request.tools.end(),
                                            [](const llm::ToolDefinition& def) {
                                                return def.name == kSkillToolName;
                                            }),
                             request.tools.end());
    }
    return request;
}

void AgentLoop::MaybeCompact() {
    if (!config_.auto_compact) return;

    // The strategy is config; the THRESHOLD is per-session, because a
    // window-relative strategy resolves against the window of the model this
    // session is currently using -- and /agent or /model can change that
    // mid-session.
    const std::int64_t trigger =
        resolve_compaction_trigger(config_.compaction_trigger, preset_.context_window_tokens);
    if (trigger <= 0) {
        using Kind = config::CompactionTriggerSettings::Kind;
        const bool window_relative = config_.compaction_trigger.kind == Kind::PercentOfWindow ||
                                     config_.compaction_trigger.kind == Kind::WindowBuffer;
        if (window_relative && !compaction_trigger_warned_) {
            compaction_trigger_warned_ = true;
            if (!preset_.context_window_tokens) {
                // Not an error state -- a local backend may genuinely not know its
                // window -- but compaction is now off for this session, and silence
                // is how that used to go unnoticed.
                session_.logger.warn(
                    "session {}: compaction trigger is window-relative but model {} declares no "
                    "context_window_tokens; auto-compaction is disabled for this session",
                    session_.session_id, preset_.model_ref.str());
            } else if (config_.compaction_trigger.kind == Kind::WindowBuffer) {
                session_.logger.warn(
                    "session {}: compaction buffer_tokens ({}) is not smaller than model {}'s "
                    "context_window_tokens ({}); auto-compaction is disabled for this session",
                    session_.session_id, config_.compaction_trigger.buffer_tokens,
                    preset_.model_ref.str(), *preset_.context_window_tokens);
            } else {
                // A percent can land here too, when it truncates to zero against a
                // very small window. Rare, but naming buffer_tokens for it -- as
                // this once did -- sends the reader to a setting they never set.
                session_.logger.warn(
                    "session {}: compaction trigger resolves to 0 against model {}'s "
                    "context_window_tokens ({}); auto-compaction is disabled for this session",
                    session_.session_id, preset_.model_ref.str(),
                    *preset_.context_window_tokens);
            }
        }
        return;
    }
    if (tokens_.last_occupancy() < trigger) return;
    Compact("auto");
}

std::optional<std::size_t> AgentLoop::CompactNow() {
    // The TRIGGER is skipped -- the user asked, so occupancy, auto_compact and
    // the configured strategy are all beside the point. Everything Compact()
    // checks is not: an empty history, a missing compactor and a summarizer that
    // already failed this turn are just as true when the request is manual.
    return Compact("manual");
}

void AgentLoop::ClearHistory() {
    history_.clear();
    // The queue is concurrency-safe by construction (its own mutex), and the
    // two sides are genuinely concurrent: /clear runs on THIS session's worker,
    // while QueueInjectedMessage runs on a NON-BLOCKING SUBAGENT's worker from
    // its completion hook -- or on a background task's watcher. A background
    // child finishing while the user types /clear is a data race on a
    // vector<Message> -- the kind that corrupts rather than crashes.
    injections_->Clear();
    // The occupancy estimate goes with the history it described. It self-heals
    // on the next RecordTurn, so leaving it stale only cost one no-op Compact()
    // -- but a token count that outlives its messages is the sort of thing a
    // later change trips over, and the compaction trigger reads it.
    tokens_ = llm::TokenAccumulator{};
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
    first_persisted_seq_.reset();
    head_is_summary_ = false;
    // Not the store. Compaction established that the in-memory working set and
    // the durable transcript are different things; /clear drops the first and
    // leaves scrollback whole.
}

void AgentLoop::TruncateAfter(std::int64_t target_seq) {
    // Nothing in memory to truncate. Reachable legitimately: a second rewind
    // after one to the transcript's start left the working set empty, and the
    // durable marks are still the Session's to make.
    if (history_.empty()) return;
    if (!first_persisted_seq_.has_value() && !head_is_summary_) {
        // No mapping to seq space. Defensive -- the Session validates the
        // target against the store before calling, and every path that fills
        // history_ also establishes the mapping -- but silent corruption
        // would be worse than an early return.
        session_.logger.warn("session {}: rewind ignored: no persisted portion to map onto",
                             session_.session_id);
        return;
    }
    // Index in history_ of the first message that must go. The contiguous-
    // suffix invariant makes this arithmetic exact: history_'s persisted
    // portion starts at first_persisted_seq_, and the synthetic summary (when
    // present) sits one slot above the arithmetic.
    const std::int64_t base = first_persisted_seq_.value_or(0);
    const std::int64_t signed_idx = target_seq - base + (head_is_summary_ ? 1 : 0);
    // A rewind to the transcript's very start leaves the working set empty.
    const std::size_t idx =
        static_cast<std::size_t>(std::max<std::int64_t>(signed_idx, 0));
    history_.erase(history_.begin() + static_cast<long>(idx), history_.end());
    if (history_.empty()) {
        // Mirrors ClearHistory: an empty working set has no persisted portion
        // to be contiguous with, and the next append establishes it.
        first_persisted_seq_.reset();
    }

    // Same reset the constructor and /clear apply: the estimate described the
    // discarded tail too.
    tokens_ = llm::TokenAccumulator{};
    if (!history_.empty()) tokens_.SeedOccupancy(estimate_tokens(history_));

    // Same queue ClearHistory clears (its own mutex, so no lock needed here):
    // a background subagent's worker can be queueing a completion while the
    // user rewinds. Those completions answer turns that no longer happened.
    injections_->Clear();

    session_.logger.info(
        "session {}: rewound to seq {}; {} messages remain in the working set",
        session_.session_id, target_seq, history_.size());
}

void AgentLoop::ReseedFromStore(std::vector<llm::Message> rows, std::int64_t first_seq) {
    history_ = std::move(rows);
    first_persisted_seq_ = history_.empty() ? std::nullopt
                                            : std::optional<std::int64_t>(first_seq);
    head_is_summary_ = false;
    tokens_ = llm::TokenAccumulator{};
    if (!history_.empty()) tokens_.SeedOccupancy(estimate_tokens(history_));
    // Same obligation TruncateAfter carries: the discarded tail owned any
    // completions queued against it.
    injections_->Clear();
    session_.logger.info(
        "session {}: rewound under the compaction boundary to seq {}; working set rebuilt "
        "with {} messages",
        session_.session_id, first_seq, history_.size());
}

void AgentLoop::RestorePersisted(std::vector<llm::Message> rows, std::int64_t first_seq) {
    if (rows.empty()) return;
    if (!first_persisted_seq_.has_value()) {
        // The working set was empty -- rewound to the transcript's start and
        // then redone. The restored rows ARE the persisted portion now.
        first_persisted_seq_ = first_seq;
        head_is_summary_ = false;
    }
    history_.insert(history_.end(), std::make_move_iterator(rows.begin()),
                    std::make_move_iterator(rows.end()));
    tokens_ = llm::TokenAccumulator{};
    tokens_.SeedOccupancy(estimate_tokens(history_));
    session_.logger.info("session {}: redo restored {} messages from seq {}",
                         session_.session_id, history_.size(), first_seq);
}

std::optional<std::size_t> AgentLoop::Compact(std::string_view trigger) {
    if (!compactor_) return std::nullopt;
    if (history_.size() < 2) return std::nullopt;
    if (compaction_failed_this_turn_) return std::nullopt;

    // Keep the tail verbatim; summarize everything older. The candidate cut is
    // chosen by count here -- the boundary snap is what makes it safe.
    std::size_t candidate = history_.size() / 2;

    // Snapping backward to a complete tool-call cycle is mandatory: cutting
    // between an assistant ToolUseBlock and its ToolResultBlock orphans the call,
    // which is a hard 400 on both OpenAI and Anthropic rather than a soft
    // degradation.
    const std::size_t cut = snap_compaction_boundary(history_, candidate);
    if (cut == 0 || cut >= history_.size()) {
        // Not a no-op worth staying quiet about. A history whose every candidate
        // boundary lands inside a tool-call cycle can never be compacted, so
        // context grows until the provider rejects the request on length -- and
        // a 400 from the backend is the only other symptom. Cooled down so this
        // says it once per turn rather than once per tool cycle.
        session_.logger.warn(
            "session {}: cannot compact -- no safe boundary in {} messages; context will keep "
            "growing",
            session_.session_id, history_.size());
        compaction_failed_this_turn_ = true;
        if (config_.hooks != nullptr) {
            config_.hooks->EmitAsync(hooks::kPostCompact,
                                     nlohmann::json{{"session_id", session_.session_id},
                                                    {"project_root", session_.project_root.string()},
                                                    {"trigger", trigger},
                                                    {"error", "no safe compaction boundary"}});
        }
        return std::nullopt;
    }

    // A SPAN, not a copy: the range being compacted is the oldest half of the
    // working history, and copying it duplicated the session's whole working
    // set of strings for the duration of the summarization call. The compactor
    // only reads, and nothing mutates history_ while it runs -- compaction is
    // sequential with the turn that triggered it, on this same worker thread.
    const std::span to_compact(history_.begin(), history_.begin() + static_cast<long>(cut));
    CompactionRequest params;
    params.model_name = preset_.provider.model_name;
    params.compression_prompt = preset_.compression_prompt;
    params.parameters = preset_.parameters;
    params.max_tokens = preset_.max_tokens;
    params.last_reported_input_tokens = tokens_.last_input_tokens();
    params.full_history = &history_;
    // The session's logger, not DefaultLogger: compaction lines belong beside
    // this loop's own compaction diagnostics, session id and all.
    params.logger = &session_.logger;
    params.session_label = session_.session_id;
    params.is_cancelled = is_cancelled_;

    // core.pre_compact: observe-only, fired once per compaction attempt, after
    // the boundary is known to be safe and before the summarizer call. Both the
    // auto trigger and /compact land here; `trigger` is filterable, since the
    // two are different events to an audit sink even when the mechanics match.
    if (config_.hooks != nullptr) {
        config_.hooks->EmitAsync(hooks::kPreCompact,
                                 nlohmann::json{{"session_id", session_.session_id},
                                                {"project_root", session_.project_root.string()},
                                                {"trigger", trigger},
                                                {"history_messages", history_.size()},
                                                {"occupancy_tokens", tokens_.last_occupancy()}});
    }

    const auto result = compactor_(to_compact, provider_.get(), params);
    if (!result) {
        // No compaction this cycle; do not retry on every tool cycle of this turn.
        compaction_failed_this_turn_ = true;
        if (config_.hooks != nullptr) {
            config_.hooks->EmitAsync(hooks::kPostCompact,
                                     nlohmann::json{{"session_id", session_.session_id},
                                                    {"project_root", session_.project_root.string()},
                                                    {"trigger", trigger},
                                                    {"error", "summarization produced no result"}});
        }
        return std::nullopt;
    }

    // Persist the VIEW checkpoint so a resume reconstructs [summary + tail]
    // instead of re-summarizing the full uncompacted transcript -- which for a
    // transcript well beyond the model window could not even fit its own
    // summarization request. The checkpoint is a cache of derived state, never
    // part of the transcript: `messages` is untouched, and a failed write is an
    // optimization miss (log and continue), because the fold below is correct
    // with or without it.
    //
    // boundary_seq arithmetic rests on one invariant: the persisted messages
    // in history_ are always a contiguous suffix of the stored transcript
    // (appends continue at MAX(seq)+1; compaction only folds from the head).
    // Persisted messages folded away = `cut` minus the synthetic head, so the
    // new first persisted seq is the boundary, +1.
    if (first_persisted_seq_.has_value()) {
        const std::int64_t persisted_removed =
            static_cast<std::int64_t>(cut) - (head_is_summary_ ? 1 : 0);
        const std::int64_t boundary_seq = *first_persisted_seq_ + persisted_removed - 1;
        try {
            store_.SaveCompactionCheckpoint(boundary_seq, result->summary_message);
        } catch (const std::exception& error) {
            // Resume falls back to the full-transcript load: today's behavior,
            // degraded only in cost, never in correctness.
            session_.logger.warn("session {}: failed to save compaction checkpoint: {}",
                                 session_.session_id, error.what());
        }
        first_persisted_seq_ = boundary_seq + 1;
        head_is_summary_ = true;
    }

    // Rebuilt with moves, not copies: the kept tail is shifted once (moving
    // strings, which is pointer swaps) rather than duplicated. The summary
    // message lands at the head, replacing everything the cut removed.
    std::vector<llm::Message> rebuilt;
    rebuilt.reserve(history_.size() - cut + 1);
    rebuilt.push_back(std::move(result->summary_message));
    std::move(history_.begin() + static_cast<long>(cut), history_.end(),
              std::back_inserter(rebuilt));
    history_.swap(rebuilt);

    // The numbers that make a compaction legible after the fact. There is no
    // wire event for this -- nothing in the protocol specifies one and no client
    // could render it yet -- so the log is the only place it exists. That
    // belongs with the TUI.
    //
    // Says "context was" rather than "compacted N tokens", because that is what
    // the number is: the size of the whole request that tripped the trigger, not
    // the size of the prefix replaced. Claiming the latter overstated it by
    // everything the cut kept.
    session_.logger.info(
        "session {}: context was ~{} tokens; compacted the oldest {} of {} messages into one "
        "summary",
        session_.session_id, result->context_tokens_before, cut, history_.size() + cut - 1);

    if (config_.hooks != nullptr) {
        config_.hooks->EmitAsync(hooks::kPostCompact,
                                 nlohmann::json{{"session_id", session_.session_id},
                                                {"project_root", session_.project_root.string()},
                                                {"trigger", trigger},
                                                {"folded_messages", cut},
                                                {"context_tokens_before",
                                                 result->context_tokens_before}});
    }

    // NO COOLDOWN ON SUCCESS, deliberately. Repeated compaction inside one turn
    // looks unbounded but is not: each pass replaces the first `cut` ~= size/2
    // messages with one, so history halves every time and `history_.size() < 2`
    // is the floor -- it terminates in log2(n) passes. Across turns, compacting
    // again once new messages have pushed occupancy back over the trigger is the
    // intended behaviour, not a runaway.

    // The VIEW produced here is checkpointed above; the transcript is not
    // touched. The store keeps the full uncompacted record, so scrollback is
    // unaffected no matter how many times this runs.
    return cut;
}

TurnResult AgentLoop::RunTurn(llm::Message seed_message) {
    TurnResult result;
    compaction_failed_this_turn_ = false;
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
    RunTurnLoop(result, /*flush_first=*/false);
    return result;
}

TurnResult AgentLoop::RunTurn() {
    TurnResult result;
    compaction_failed_this_turn_ = false;
    // The wake path: no seed of its own -- the injected messages ARE the user
    // turn, flushed at the top of the loop body.
    RunTurnLoop(result, /*flush_first=*/true);
    return result;
}

void AgentLoop::RunTurnLoop(TurnResult& result, bool flush_first) {
    if (flush_first) FlushInjectedMessages();
    int turns = 0;
    while (true) {
        if (Cancelled()) {
            result.stop_reason = llm::StopReason::Cancelled;
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
        const llm::ChatRequest chat_request = BuildChatRequest();
        // Set before the call, not from the response: a turn that fails or is
        // cancelled before any response still reports the model it was aimed
        // at, and TurnSummary::model is populated on every exit path.
        result.model = chat_request.model;
        llm::ChatResponse response;
        bool got_response = false;
        bool cancelled_in_backoff = false;
        for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
            try {
                response = provider_.get().StreamComplete(chat_request, event_sink_);
                got_response = true;
                break;
            } catch (const llm::LlmError& error) {
                // Retry policy lives here, not in the provider: only the loop
                // knows whether another attempt is still wanted.
                if (!error.is_retryable() || attempt == config_.max_retries) {
                    throw;
                }
                // Backoff, not a bare retry. Without it the four attempts land
                // inside a few milliseconds, which turns a 429 into four 429s
                // and is worse for the backend than not retrying at all.
                if (!SleepForBackoff(attempt)) {
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
        tokens_.RecordTurn(response.usage);
        result.stop_reason = response.stop_reason;

        // MOVED, not copied. `response` is loop-local and dead after this point
        // -- the next iteration reassigns it -- and content can be the whole
        // assistant turn, so this was a full duplicate of every block and string
        // on the hottest path there is. The tool_uses pointers below stay valid
        // across the later move into history_: a vector move relocates the
        // heap buffer's ownership, not the elements.
        llm::Message assistant{llm::Role::Assistant, std::move(response.content)};
        result.final_text = TextOf(assistant);
        const auto tool_uses = ToolUsesOf(assistant);
        // The only per-message token count that exists: this response's output.
        AppendAndPersist(std::move(assistant), response.usage.output_tokens);

        if (response.stop_reason == llm::StopReason::Cancelled) break;

        // Anything other than ToolUse ends the turn. MaxTokens in particular is
        // reported as itself rather than collapsed into EndTurn -- otherwise
        // output is truncated mid-sentence and the turn looks successful.
        if (response.stop_reason != llm::StopReason::ToolUse) break;

        if (tool_uses.empty()) break;  // ToolUse with no blocks: nothing to do

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
                                 ctx.allowed_skills = preset_.allowed_skills;
                                 ctx.is_cancelled = is_cancelled_;
                                 ctx.cancel_token = cancel_token_;
                                 // Per invocation, from the Session-owned window: a
                                 // subagent sees only its own transcript, because the
                                 // window is bound to THIS session's id
                                 // (docs/EXTENSIBILITY.md §2.1).
                                 ctx.history = session_.history;
                                  if (session_.tool_output_sink) {
                                      const std::string tool_use_id = use->id;
                                      ctx.emit_output = [this, tool_use_id](std::string_view chunk) {
                                          session_.tool_output_sink(tool_use_id, chunk);
                                      };
                                  }
                                  // Deferred-completion surface, copied verbatim from the session's
                                  // wiring (see SessionContext): a tool that schedules work beyond
                                  // its own invocation captures these, and their lifetime is the
                                  // session's shared channels block -- not this call, not this loop.
                                  ctx.schedule_injection = session_.injection_sink;
                                  ctx.session_lifetime = session_.session_lifetime;
                                  ctx.emit_task_event = session_.task_event_sink;
                                  return ctx;
                              }(),
                              false};

            // Recorded at gate time, in block order: what ran is the question,
            // not what succeeded, and call order is the model's order -- not
            // the order executions happen to finish in.
            result.tool_names_invoked.push_back(use->name);

            call.prepared = executor_.Prepare(use->id, use->name, use->input, call.ctx);
            // Unknown tools arrive here with no resolved tool; they execute as
            // their denial result and have no exclusivity to protect.
            call.to_pool = !call.prepared.tool || call.prepared.tool->parallel_safe();
            calls.push_back(std::move(call));
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
            if (session_.tool_lifecycle_sink) session_.tool_lifecycle_sink(lifecycle);

            const auto tool_started = std::chrono::steady_clock::now();
            llm::ToolResultBlock tool_result = executor_.Execute(call.prepared, call.ctx);

            // Emitted even when Execute() reports an error, and before the
            // result is appended: a client that showed a tool as started must
            // always see it finish, or its running-tools panel accumulates
            // entries that never clear.
            lifecycle.started = false;
            lifecycle.is_error = tool_result.is_error;
            lifecycle.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - tool_started);
            if (session_.tool_lifecycle_sink) session_.tool_lifecycle_sink(lifecycle);

            results[call.index] = std::move(tool_result);
        };

        // Parallel-safe calls go to the pool first, so the serial ones below
        // overlap as much of the batch as possible. Serial-only tools
        // (parallel_safe() == false) run here on the worker, one at a time, in
        // block order: their contract is "my invocations never overlap each
        // other", and the worker is the thread that guarantees it.
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

        AppendAndPersist(llm::Message{llm::Role::User, std::move(results)});

        // Turn boundary: safe to splice in anything a background subagent
        // finished while this turn was running.
        FlushInjectedMessages();
    }

    FlushInjectedMessages();
}

}  // namespace minimal::agent
