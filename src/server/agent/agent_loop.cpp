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
      compactor_(std::move(compactor)),
      compaction_strategies_(compaction_strategies),
      working_(store, std::move(seed.history), seed.first_persisted_seq, seed.head_is_summary),
      request_(preset_, std::move(project_context), registry_, config_, working_.messages(),
               working_.tokens(), is_cancelled_, session_.session_id),
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
            DropStaleThinking(1, std::min(working_.messages().size(), static_cast<std::size_t>(kept) + 1));
        }
    }
}

void AgentLoop::DropStaleThinking(std::size_t first, std::size_t last) {
    if (!provider_.get().BindsThinkingToPrefix()) return;
    working_.DropThinking(first, last);
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
    // The larger of the backend's last report and a fresh estimate. The report
    // lags a round: it describes the PREVIOUS request, so a tool result (or
    // mention) appended since is invisible to it, and one huge result would
    // sail past the trigger straight into a window overflow.
    const std::int64_t occupancy = std::max(working_.tokens().last_occupancy(), EstimateContextTokens());
    if (occupancy < trigger) {
        // Back under the trigger: the "right after a compaction" window is
        // over, and a halted auto-compaction re-arms.
        auto_compacted_last_check_ = false;
        auto_compaction_halted_ = false;
        return;
    }
    if (auto_compaction_halted_) return;

    // GUARD 1: the trigger must sit above the part of the context no
    // compaction can remove -- system prompt, project context, tool
    // definitions. At or below it, every fold leaves the context over the
    // trigger and the next check fires again.
    const SystemAndTools system_and_tools = BuildSystemAndTools();
    const std::int64_t fixed_tokens =
        EstimateContextTokens(system_and_tools.system, system_and_tools.tools) -
        estimate_tokens(working_.messages());
    if (trigger <= fixed_tokens) {
        if (!compaction_floor_warned_) {
            compaction_floor_warned_ = true;
            EmitCompactionNotice(
                "auto", "auto-compaction disabled: the trigger (" + std::to_string(trigger) +
                            " tokens) is not above the fixed context (system prompt, project "
                            "context, tools: ~" +
                            std::to_string(fixed_tokens) +
                            " tokens), which no compaction can remove; raise "
                            "memory.context_compaction_trigger");
        }
        return;
    }

    // GUARD 2: over the trigger again at the very next check after an auto
    // compaction -- the fold did not get the context under the trigger (or
    // one round refilled it). Compacting again would summarize every tool
    // round for little gain; halt with a notice until occupancy drops below
    // the trigger (a manual /compact or /clear, or a smaller next response).
    if (auto_compacted_last_check_) {
        auto_compaction_halted_ = true;
        EmitCompactionNotice(
            "auto", "auto-compaction stopped: the context (~" +
                        std::to_string(occupancy) +
                        " tokens) is still over the trigger (" + std::to_string(trigger) +
                        " tokens) right after the previous compaction; run /compact, raise the "
                        "trigger, or raise memory.compaction_replace_percent");
        return;
    }
    auto_compacted_last_check_ = Compact("auto").has_value();
}

void AgentLoop::EmitCompactionNotice(std::string_view trigger, std::string warning) {
    session_.logger.warn("session {}: {}", session_.session_id, warning);
    CompactionEvent event;
    event.trigger = std::string(trigger);
    event.skipped = true;
    event.warning = std::move(warning);
    event.tokens_before = working_.tokens().last_occupancy();
    event.tokens_after = event.tokens_before;
    event.created_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    if (working_.first_persisted_seq().has_value()) {
        event.at_seq = store_.last_appended_seq();
        try {
            // A notice record: boundary -1, never the checkpoint (skipped rows
            // are stored inactive), but scrollback renders it where it happened.
            store_.SaveCompactionCheckpoint(
                -1, llm::Message{llm::Role::User, {}},
                session::CompactionMeta{0, event.tokens_before, event.tokens_after, event.trigger,
                                        event.warning, true});
        } catch (const std::exception& error) {
            session_.logger.warn("session {}: failed to record compaction notice: {}",
                                 session_.session_id, error.what());
        }
    }
    if (session_.compaction_sink) {
        try {
            session_.compaction_sink(event);
        } catch (const std::exception& error) {
            session_.logger.warn("session {}: compaction notice dropped: {}",
                                 session_.session_id, error.what());
        }
    }
}

std::optional<std::size_t> AgentLoop::CompactNow(std::string_view prompt_override) {
    // The TRIGGER is skipped -- the user asked, so occupancy, auto_compact and
    // the configured strategy are all beside the point. Everything Compact()
    // checks is not: an empty history, a missing compactor and a summarizer that
    // already failed this turn are just as true when the request is manual.
    return Compact("manual", prompt_override);
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

std::optional<std::size_t> AgentLoop::Compact(std::string_view trigger,
                                              std::string_view prompt_override) {
    // Resolve the summarizer NOW, not at construction: an injected compactor
    // (tests, direct-API users) wins outright; otherwise the preset's
    // `compaction_strategy` frontmatter names the strategy -- resolved against
    // the registry at each attempt, so a mid-session /agent switch changes the
    // summary format the way it already changes compression_prompt -- and the
    // settings-level strategy is the fallback. See EffectiveCompactor's
    // precedence doc; the unknown-preset-name warn lives there.
    Compactor summarizer = compactor_;
    if (!summarizer) {
        summarizer = config_.EffectiveCompactor(preset_, compaction_strategies_);
        if (summarizer && !preset_.compaction_strategy.empty() && compaction_strategies_ != nullptr) {
            // The preset named a kind nothing registered, OR named one that IS
            // registered but declares no summarize half (a cut-only plugin
            // strategy referenced by mistake) -- EffectiveCompactor falls
            // through to the settings-level strategy or the built-in in both
            // cases, and both deserve the same operator-visible signal: a
            // silent fallback here is indistinguishable from the preset's
            // choice actually taking effect.
            const auto* found = compaction_strategies_->Find(preset_.compaction_strategy);
            if (found == nullptr || !found->summarize) {
                // Warn ONCE per loop, not per attempt -- a failing summarizer
                // already has a per-turn cooldown, but the name is a config
                // typo, not a transient condition, so per-attempt repetition
                // is log spam.
                if (!compaction_strategy_warned_) {
                    compaction_strategy_warned_ = true;
                    session_.logger.warn(
                        "session {}: agent preset '{}' names compaction strategy '{}' which is "
                        "not usable ({}); using the settings-level strategy or the built-in",
                        session_.session_id, preset_.name, preset_.compaction_strategy,
                        found == nullptr ? "not registered" : "registered with no summarizer");
                }
            }
        }
    }
    if (!summarizer) return std::nullopt;
    if (working_.messages().size() < 2) return std::nullopt;
    if (compaction_failed_this_turn_) return std::nullopt;

    // Every exit from here on defaults to "failed this turn" -- only the
    // success return at the bottom clears it, replacing what used to be a
    // separate `compaction_failed_this_turn_ = true;` at each of the four
    // failure exits below (easy to forget on a future one).
    struct FailureGuard {
        bool& flag;
        bool succeeded = false;
        ~FailureGuard() {
            if (!succeeded) flag = true;
        }
    } failure_guard{compaction_failed_this_turn_};

    // Keep the tail verbatim; summarize everything older. The candidate cut
    // comes from the configured strategy's cut selector (S4) when one is
    // set -- honoring memory.compaction_keep_recent_tokens (W11) via a
    // token-budget walk from the tail, instead of the legacy plain halving
    // -- else the boundary snap below is what makes ANY candidate safe.
    //
    // compaction_replace_percent is expressed to the selector as the
    // keep-recent budget it already understands: (100 - percent)% of the
    // working history's token estimate stays verbatim. Counting messages (the
    // legacy halving) kept ~40% of a tool-heavy context, whose newest
    // messages carry most of the bytes. An explicit keep_recent_tokens wins.
    std::int64_t keep_recent_tokens = config_.compaction_keep_recent_tokens;
    if (keep_recent_tokens <= 0 && config_.compaction_replace_percent > 0) {
        const std::int64_t total = estimate_tokens(working_.messages());
        keep_recent_tokens =
            std::max<std::int64_t>(1, total * (100 - config_.compaction_replace_percent) / 100);
    }
    // Strategy code is plugin code: a throwing selector or summarizer fails
    // the COMPACTION (flagged by failure_guard, the turn continues
    // uncompacted), not every turn that crosses the trigger.
    std::size_t candidate = 0;
    try {
        candidate = config_.compaction_strategy.cut_selector
                        ? config_.compaction_strategy.cut_selector(working_.messages(), keep_recent_tokens)
                    : keep_recent_tokens > 0 ? ProposeSummarizeCut(working_.messages(), keep_recent_tokens)
                                             : working_.messages().size() / 2;
    } catch (const std::exception& error) {
        session_.logger.warn("session {}: compaction cut selector threw: {}", session_.session_id,
                             error.what());
        return std::nullopt;
    }

    // The STRATEGY declining to cut anything is a DIFFERENT situation from
    // "no safe structural cut exists" below, and must not be handled by the
    // same fallback. If compaction_keep_recent_tokens is set at or above the
    // resolved trigger, the cut selector's own token-budget walk covers the
    // whole working set and returns 0 here -- BEFORE the boundary snap even
    // runs, so this has nothing to do with tool-call pairing. Falling
    // through to the boundary-safety branch used to run the elision
    // fallback on every turn (silently truncating a tool result) and log
    // "no safe compaction boundary" for a misconfiguration that has no
    // boundary problem at all.
    if (candidate == 0) {
        session_.logger.info(
            "session {}: compaction_keep_recent_tokens covers the whole working set ({} "
            "messages); nothing to compact",
            session_.session_id, working_.messages().size());
        if (config_.hooks != nullptr) {
            config_.hooks->EmitAsync(
                hooks::kPostCompact,
                nlohmann::json{{"session_id", session_.session_id},
                              {"project_root", session_.project_root.string()},
                              {"trigger", trigger},
                              {"skipped", "compaction_keep_recent_tokens covers the whole "
                                          "working set; nothing to compact"}});
        }
        return std::nullopt;
    }

    // Snapping backward to a complete tool-call cycle is mandatory: cutting
    // between an assistant ToolUseBlock and its ToolResultBlock orphans the call,
    // which is a hard 400 on both OpenAI and Anthropic rather than a soft
    // degradation. This runs UNCONDITIONALLY regardless of where `candidate`
    // came from -- a strategy's proposed cut is a starting point, never a
    // guarantee, and that invariant is therefore not a contract a strategy
    // author has to remember or can get wrong.
    const std::size_t cut = snap_compaction_boundary(working_.messages(), candidate);
    if (cut == 0 || cut >= working_.messages().size()) {
        // No safe structural cut. Rather than accept unbounded growth, try to
        // shrink the context directly: elide the largest tool result in
        // place (see ElideLargestToolResult) -- this never disturbs message
        // boundaries, so it cannot make the pairing problem worse, and it
        // addresses the actual harm (context SIZE) even though it does not
        // fold any messages together.
        if (ElideLargestToolResult(working_.messages())) {
            working_.tokens().SeedOccupancy(EstimateContextTokens());
            session_.logger.warn(
                "session {}: no safe compaction boundary in {} messages; elided the largest "
                "tool result instead",
                session_.session_id, working_.messages().size());
            if (config_.hooks != nullptr) {
                config_.hooks->EmitAsync(
                    hooks::kPostCompact,
                    nlohmann::json{{"session_id", session_.session_id},
                                  {"project_root", session_.project_root.string()},
                                  {"trigger", trigger},
                                  {"error", "no safe compaction boundary; elided largest tool result"}});
            }
            return std::nullopt;
        }

        // The genuine last resort: no safe boundary AND nothing left worth
        // eliding (every tool result is already small). Not a no-op worth
        // staying quiet about -- context grows until the provider rejects the
        // request on length, and a 400 from the backend is the only other
        // symptom. Cooled down so this says it once per turn rather than once
        // per tool cycle.
        session_.logger.warn(
            "session {}: cannot compact -- no safe boundary in {} messages; context will keep "
            "growing",
            session_.session_id, working_.messages().size());
        if (config_.hooks != nullptr) {
            config_.hooks->EmitAsync(hooks::kPostCompact,
                                     nlohmann::json{{"session_id", session_.session_id},
                                                    {"project_root", session_.project_root.string()},
                                                    {"trigger", trigger},
                                                    {"error", "no safe compaction boundary"}});
        }
        return std::nullopt;
    }

    // The summarizer reads the WHOLE working history; only [0, cut) is
    // replaced by its summary. A summary written knowing the recent messages
    // describes the current state rather than the state at the cut, and the
    // request -- the session's own system/tools/history plus one appended
    // instruction -- is a prefix-cache hit on everything the last request
    // sent. A SPAN, not a copy: the compactor only reads, and nothing mutates
    // working_.messages() while it runs -- compaction is sequential with the turn that
    // triggered it, on this same worker thread.
    const std::span<const llm::Message> to_compact(working_.messages());
    CompactionRequest params;
    params.model_name = preset_.provider.model_name;
    // A non-empty override (/compact <name>) wins for THIS invocation only --
    // auto-compact and bare /compact keep the preset's prompt.
    params.compression_prompt =
        prompt_override.empty() ? preset_.compression_prompt : prompt_override;
    params.custom_prompt = !prompt_override.empty() || preset_.custom_compression_prompt;
    params.structured_prompt = preset_.structured_compression_prompt;
    {
        SystemAndTools system_and_tools = BuildSystemAndTools();
        params.system = std::move(system_and_tools.system);
        params.tools = std::move(system_and_tools.tools);
    }
    params.parameters = preset_.parameters;
    params.max_tokens = preset_.max_tokens;
    // The summarizer call sticks to the same routing key as the conversation
    // it summarizes -- see CompactionRequest::session_id.
    params.session_id = session_.session_id;
    // Whole-context occupancy, not last_input_tokens(): with prompt caching
    // the uncached input is a small fraction of the context, which is how a
    // 700k context used to log as a few thousand tokens.
    params.last_reported_input_tokens = working_.tokens().last_occupancy() > 0
                                            ? working_.tokens().last_occupancy()
                                            : EstimateContextTokens(params.system, params.tools);
    params.full_history = &working_.messages();
    // The session's logger, not DefaultLogger: compaction lines belong beside
    // this loop's own compaction diagnostics, session id and all.
    params.logger = &session_.logger;
    params.session_label = session_.session_id;
    params.is_cancelled = is_cancelled_;

    // small_model, when configured: the summarizer ALONE runs on the alternate
    // provider -- its model name, parameters and cap; the session's own model
    // still answers every turn. A POINTER, not a reference: re-seating a
    // reference is impossible, and an assignment through one would run the
    // small provider's copy-assignment against the session's own provider --
    // corrupting it for every later turn -- rather than switching which
    // provider the summarizer call uses. Named summarizer_provider to stay
    // distinct from the summarizer std::function above.
    llm::LlmProvider* summarizer_provider = &provider_.get();
    if (config_.small_model.has_value() && config_.small_model->provider != nullptr) {
        summarizer_provider = config_.small_model->provider.get();
        params.model_name = config_.small_model->model_name;
        params.parameters = config_.small_model->parameters;
        params.max_tokens = config_.small_model->max_tokens;
    }
    // The summarizer request carries the whole history too, so the same
    // input + max_tokens <= window rule applies -- against the window of
    // whichever model serves it.
    {
        const bool small = summarizer_provider != &provider_.get();
        const std::optional<std::int64_t> window =
            small ? config_.small_model->context_window_tokens : preset_.context_window_tokens;
        const std::int64_t input_tokens = std::max(
            params.last_reported_input_tokens, EstimateContextTokens(params.system, params.tools));
        params.max_tokens = FitOutputCap(params.max_tokens, window, input_tokens);
    }

    // core.pre_compact: observe-only, fired once per compaction attempt, after
    // the boundary is known to be safe and before the summarizer call. Both the
    // auto trigger and /compact land here; `trigger` is filterable, since the
    // two are different events to an audit sink even when the mechanics match.
    if (config_.hooks != nullptr) {
        config_.hooks->EmitAsync(hooks::kPreCompact,
                                 nlohmann::json{{"session_id", session_.session_id},
                                                {"project_root", session_.project_root.string()},
                                                {"trigger", trigger},
                                                {"history_messages", working_.messages().size()},
                                                {"occupancy_tokens", working_.tokens().last_occupancy()}});
    }

    std::optional<CompactionResult> result;
    try {
        result = summarizer(to_compact, *summarizer_provider, params);
    } catch (const std::exception& error) {
        session_.logger.warn("session {}: compaction summarizer threw: {}", session_.session_id,
                             error.what());
    }
    if (!result) {
        // No compaction this cycle; do not retry on every tool cycle of this turn.
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
    // in working_.messages() are always a contiguous suffix of the stored transcript
    // (appends continue at MAX(seq)+1; compaction only folds from the head).
    // Persisted messages folded away = `cut` minus the synthetic head, so the
    // new first persisted seq is the boundary, +1.
    // The history's share of the context before and after, for GUARD 3 below
    // -- estimates on both sides (bytes/4), so the comparison is like for like,
    // with the fixed part (system prompt, tools) taken out: it is the same on
    // both sides and would make any fold look ineffective at small sizes.
    const std::int64_t history_tokens_before = estimate_tokens(working_.messages());
    const std::int64_t fixed_tokens =
        EstimateContextTokens(params.system, params.tools) - history_tokens_before;

    CompactionEvent event;
    event.trigger = std::string(trigger);
    event.folded_messages = static_cast<std::int64_t>(cut);
    // Before and after on ONE basis -- the loop's estimate of the context as
    // the fold sees it. The backend-reported occupancy is the previous
    // response's, so it lags the tool results appended since (a fold could
    // show as "10.7k -> 10.3k" while the history actually halved), and it is
    // in real tokens where the after-figure can only be estimated. Not the
    // compactor's number either: a plugin compactor may report anything.
    event.tokens_before = fixed_tokens + history_tokens_before;
    event.summary_text = llm::TextOf(result->summary_message.content);
    event.created_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();

    // The summary is now the head WHATEVER the persisted bookkeeping did:
    // a loop constructed without a first-persisted seq (the direct-API case;
    // every Session seeding path sets one) still folds the summary into
    // working_.messages()[0], and leaving working_.head_is_summary() false would make every later
    // seq mapping -- and CheckpointBoundary() -- mis-count the synthetic head.
    std::optional<std::int64_t> boundary_seq;
    if (working_.first_persisted_seq().has_value()) {
        const std::int64_t persisted_removed =
            static_cast<std::int64_t>(cut) - (working_.head_is_summary() ? 1 : 0);
        boundary_seq = *working_.first_persisted_seq() + persisted_removed - 1;
        working_.first_persisted_seq() = *boundary_seq + 1;
    }
    working_.head_is_summary() = true;

    // Rebuilt with moves, not copies: the kept tail is shifted once (moving
    // strings, which is pointer swaps) rather than duplicated. The summary
    // message lands at the head, replacing everything the cut removed.
    std::vector<llm::Message> rebuilt;
    rebuilt.reserve(working_.messages().size() - cut + 1);
    rebuilt.push_back(result->summary_message);
    std::move(working_.messages().begin() + static_cast<long>(cut), working_.messages().end(),
              std::back_inserter(rebuilt));
    working_.messages().swap(rebuilt);

    // The kept tail's THINKING goes, on backends that bind it to its prefix
    // (working set only; the transcript keeps it). Its blocks were produced
    // against the full, pre-fold prefix, and newer Claude models reject a
    // replay after that prefix changed ("preserved thinking" -- keep-tail
    // compaction is the documented breaking shape). Text and tool calls stay.
    // Chat-completions backends never replay thinking, so nothing changes.
    DropStaleThinking(1, working_.messages().size());

    // RE-SEED the occupancy estimate: the one site that never did. The fold
    // just shrank the working set, but the accumulator still described the
    // pre-fold context -- so the next MaybeCompact (the same turn's next tool
    // cycle, or the next turn before any response reported usage) fired on a
    // number that no longer existed. A manual /compact was guaranteed one
    // spurious second summarization; an auto one could chain. SeedOccupancy
    // touches only the last-turn estimate, so the backend-reported counts the
    // accumulator carries stay intact.
    event.tokens_after = EstimateContextTokens();
    working_.tokens().SeedOccupancy(event.tokens_after);

    // GUARD 3: a fold that barely shrank the history (the summary is about as
    // large as what it replaced, or the kept tail is most of the history).
    // Not refused -- the summary is already in -- but said out loud.
    const std::int64_t history_tokens_after = event.tokens_after - fixed_tokens;
    if (history_tokens_before > 0 && history_tokens_after * 10 >= history_tokens_before * 9) {
        event.warning = "compaction freed little: the history went from ~" +
                        std::to_string(history_tokens_before) + " to ~" +
                        std::to_string(history_tokens_after) +
                        " tokens (plus ~" + std::to_string(fixed_tokens) +
                        " tokens of system prompt and tools)";
        session_.logger.warn("session {}: {}", session_.session_id, event.warning);
    }

    // Persist the record (the checkpoint resume seeds from, and the row
    // scrollback renders) only now that the after-size is known.
    if (boundary_seq.has_value()) {
        event.boundary_seq = *boundary_seq;
        event.at_seq = store_.last_appended_seq();
        try {
            store_.SaveCompactionCheckpoint(
                *boundary_seq, result->summary_message,
                session::CompactionMeta{event.folded_messages, event.tokens_before,
                                        event.tokens_after, event.trigger, event.warning});
        } catch (const std::exception& error) {
            // Resume falls back to the full-transcript load: today's behavior,
            // degraded only in cost, never in correctness.
            session_.logger.warn("session {}: failed to save compaction checkpoint: {}",
                                 session_.session_id, error.what());
        }
    }

    // "context was" rather than "compacted N tokens", because that is what the
    // number is: the size of the whole context that tripped the trigger, not
    // the size of the prefix replaced.
    session_.logger.info(
        "session {}: context was ~{} tokens, now ~{}; compacted the oldest {} of {} messages "
        "into one summary",
        session_.session_id, event.tokens_before, event.tokens_after, cut,
        working_.messages().size() + cut - 1);

    if (session_.compaction_sink) {
        try {
            session_.compaction_sink(event);
        } catch (const std::exception& error) {
            session_.logger.warn("session {}: compaction event dropped: {}", session_.session_id,
                                 error.what());
        }
    }

    if (config_.hooks != nullptr) {
        config_.hooks->EmitAsync(hooks::kPostCompact,
                                 nlohmann::json{{"session_id", session_.session_id},
                                                {"project_root", session_.project_root.string()},
                                                {"trigger", trigger},
                                                {"folded_messages", cut},
                                                {"context_tokens_before", event.tokens_before},
                                                {"context_tokens_after", event.tokens_after}});
    }

    // NO COOLDOWN ON SUCCESS, deliberately. Repeated compaction inside one turn
    // looks unbounded but is not: each pass replaces at least one message (the
    // cut is >= 1) and the estimate is re-seeded smaller, and `working_.messages().size()
    // < 2` is the floor. Across turns, compacting
    // again once new messages have pushed occupancy back over the trigger is the
    // intended behaviour, not a runaway.

    // The VIEW produced here is checkpointed above; the transcript is not
    // touched. The store keeps the full uncompacted record, so scrollback is
    // unaffected no matter how many times this runs.
    failure_guard.succeeded = true;
    return cut;
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
    compaction_failed_this_turn_ = false;
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
    compaction_failed_this_turn_ = false;
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
