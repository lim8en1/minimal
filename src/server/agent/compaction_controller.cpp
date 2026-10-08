#include "minimal/agent/compaction_controller.hpp"

#include <algorithm>
#include <chrono>
#include <limits>

#include "minimal/agent/compaction_boundary.hpp"
#include "minimal/agent/compaction_trigger.hpp"
#include "minimal/agent/context_compactor.hpp"
#include "minimal/agent/transcript_repair.hpp"
#include "minimal/common/utf8.hpp"
#include "minimal/agent/policy.hpp"
#include "minimal/common/units.hpp"
#include "tool_result_caps.hpp"

namespace minimal::agent {

void CompactionController::DropStaleThinking(std::size_t first, std::size_t last) {
    if (!provider_.get().BindsThinkingToPrefix()) return;
    working_.DropThinking(first, last);
}

void CompactionController::MaybeCompact() {
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
    const std::int64_t occupancy = std::max(working_.tokens().last_occupancy(), request_.EstimateContextTokens());
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
    const std::int64_t fixed_tokens = request_.FixedContextTokens();
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

void CompactionController::EmitCompactionNotice(std::string_view trigger, std::string warning) {
    session_.logger.warn("session {}: {}", session_.session_id, warning);
    CompactionEvent event;
    event.trigger = std::string(trigger);
    event.skipped = true;
    event.warning = std::move(warning);
    // The estimate, like a compaction that ran (Compact's tokens_before): one
    // column, one basis -- a skipped row used to store the reported
    // occupancy beside successful rows' estimates.
    event.tokens_before = request_.EstimateContextTokens();
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

std::optional<std::size_t> CompactionController::CompactNow(std::string_view prompt_override) {
    // The TRIGGER is skipped -- the user asked, so occupancy, auto_compact and
    // the configured strategy are all beside the point. Everything Compact()
    // checks is not: an empty history, a missing compactor and a summarizer that
    // already failed this turn are just as true when the request is manual.
    return Compact("manual", prompt_override);
}

// Resolved at each attempt, not at construction (see below). nullopt-equivalent
// (an empty Compactor) when no summarizer is available.
Compactor CompactionController::ResolveSummarizer() {
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
    return summarizer;
}

// Picks where to fold: the strategy's (or the default token-budget) candidate,
// snapped back to a complete tool-call cycle. nullopt means nothing is to be
// compacted this time -- each such exit has already logged and emitted its
// post-compact hook (or elided an oversized tool result in place as the
// no-safe-boundary fallback).
std::optional<std::size_t> CompactionController::ChooseCut(std::string_view trigger) {
    // compaction_replace_percent is expressed to the selector as the
    // keep-recent budget it already understands: (100 - percent)% of the
    // working history's token estimate stays verbatim. An explicit
    // keep_recent_tokens wins; with neither, the cut is by message count.
    std::int64_t keep_recent_tokens = config_.compaction_keep_recent_tokens;
    if (keep_recent_tokens <= 0 && config_.compaction_replace_percent > 0) {
        const std::int64_t total = working_.estimated_tokens();
        keep_recent_tokens =
            std::max<std::int64_t>(1, total - common::PercentOf(total, config_.compaction_replace_percent));
    }
    // Strategy code is plugin code: a throwing selector or summarizer fails
    // the COMPACTION (flagged by failure_guard, the turn continues
    // uncompacted), not every turn that crosses the trigger.
    std::size_t candidate = 0;
    try {
        candidate = config_.compaction_strategy.cut_selector
                        ? config_.compaction_strategy.cut_selector(working_.messages(), keep_recent_tokens)
                        : ProposeSummarizeCut(working_.messages(), keep_recent_tokens);
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
        if (working_.MutateInPlace(
                [](std::vector<llm::Message>& messages) { return ElideLargestToolResult(messages); })) {
            working_.tokens().SeedOccupancy(request_.EstimateContextTokens());
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
    return cut;
}

std::optional<std::size_t> CompactionController::Compact(std::string_view trigger,
                                              std::string_view prompt_override) {
    Compactor summarizer = ResolveSummarizer();
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
    // set, else the summarize token-budget walk from the tail; the boundary
    // snap below is what makes ANY candidate safe.
    //
    const std::optional<std::size_t> chosen_cut = ChooseCut(trigger);
    if (!chosen_cut) return std::nullopt;
    const std::size_t cut = *chosen_cut;

    // THE SUMMARIZER MUST FIT. It re-sends the whole history, so a context
    // already past the window (one oversized tool result is enough) makes
    // the rescue fail exactly like the request it was meant to rescue, and
    // the session can neither continue nor compact. Elide the largest tool
    // results in place until the request fits with the headroom kept free --
    // the persisted transcript and the spill files keep the full text, and
    // eliding never moves a message, so `cut` stays valid. After a provider
    // overflow the context's size is unknown but known to be too big: the
    // estimate counts at kConservativeBytesPerToken there, since dense
    // output is what undercounts.
    {
        const bool small = config_.small_model.has_value() && config_.small_model->provider != nullptr;
        const std::optional<std::int64_t> window =
            small ? config_.small_model->context_window_tokens : preset_.context_window_tokens;
        if (window && *window > 0) {
            const std::int64_t limit = *window - RequestBuilder::CompactionHeadroomTokens(*window);
            const bool overflowed = trigger == "overflow";
            const auto context_tokens = [&] {
                const std::int64_t estimate = request_.EstimateContextTokens();
                return overflowed ? estimate * kEstimatedBytesPerToken / kConservativeBytesPerToken
                                  : std::max(estimate, working_.tokens().last_occupancy());
            };
            std::size_t elided = 0;
            while (context_tokens() > limit &&
                   working_.MutateInPlace([](std::vector<llm::Message>& messages) {
                       return ElideLargestToolResult(messages);
                   })) {
                ++elided;
                // The report described the context before this elision.
                working_.tokens().SeedOccupancy(request_.EstimateContextTokens());
            }
            if (elided > 0) {
                session_.logger.warn(
                    "session {}: elided {} tool result(s) so the compaction request fits the "
                    "{}-token window (the full text stays in the transcript and spill files)",
                    session_.session_id, elided, *window);
            }
        }
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
    {
        RequestBuilder::SystemAndTools system_and_tools = request_.BuildSystemAndTools();
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
                                            : request_.EstimateContextTokens(params.system, params.tools);
    params.full_history = &working_.messages();
    // The session's logger, not DefaultLogger: compaction lines belong beside
    // this loop's own compaction diagnostics, session id and all.
    params.logger = &session_.logger;
    params.session_label = session_.session_id;
    params.is_cancelled = is_cancelled_;
    // Billed whatever becomes of the summary. model_name is final below
    // (small_model's when one serves), so the hook reads it at call time.
    if (config_.on_billed_call) {
        params.on_usage = [this, &params](const llm::Usage& usage) {
            config_.on_billed_call(BilledCall{std::string(params.model_name), usage,
                                              !llm::ReportedUsage(usage), "compaction"});
        };
    }

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
            params.last_reported_input_tokens, request_.EstimateContextTokens(params.system, params.tools));
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
    const std::int64_t history_tokens_before = working_.estimated_tokens();
    const std::int64_t fixed_tokens =
        request_.EstimateContextTokens(params.system, params.tools) - history_tokens_before;

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
    working_.MutateInPlace([&](std::vector<llm::Message>& messages) {
        std::vector<llm::Message> rebuilt;
        rebuilt.reserve(messages.size() - cut + 1);
        rebuilt.push_back(result->summary_message);
        std::move(messages.begin() + static_cast<long>(cut), messages.end(),
                  std::back_inserter(rebuilt));
        messages.swap(rebuilt);
    });

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
    event.tokens_after = request_.EstimateContextTokens();
    working_.tokens().SeedOccupancy(event.tokens_after);

    // GUARD 3: a fold that barely shrank the history (the summary is about as
    // large as what it replaced, or the kept tail is most of the history).
    // Not refused -- the summary is already in -- but said out loud.
    const std::int64_t history_tokens_after = event.tokens_after - fixed_tokens;
    if (history_tokens_before > 0 &&
        history_tokens_after >= common::PercentOf(history_tokens_before, kCompactionFreedLittlePercent)) {
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
    folds_.fetch_add(1, std::memory_order_release);
    return cut;
}

}  // namespace minimal::agent
