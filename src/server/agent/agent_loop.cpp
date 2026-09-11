#include "minimal/agent/agent_loop.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>
#include <variant>

#include "minimal/agent/compaction_boundary.hpp"
#include "minimal/agent/compaction_trigger.hpp"
#include "minimal/agent/transcript_repair.hpp"

namespace minimal::agent {
namespace {

std::vector<const llm::ToolUseBlock*> ToolUsesOf(const llm::Message& message) {
    std::vector<const llm::ToolUseBlock*> out;
    for (const auto& block : message.content) {
        if (const auto* use = std::get_if<llm::ToolUseBlock>(&block)) {
            out.push_back(use);
        }
    }
    return out;
}

// THE global tool-RESULT ceiling (settings.json tools.max_result_bytes), the
// output-side mirror of the input ceiling checked before Prepare. Applied
// here, once, after Execute() returns, so it covers every tool uniformly --
// a core tool's own cap (bash.max_output_bytes) is the plugin's choice, not
// a harness guarantee, and an MCP tool or third-party plugin tool has no cap
// at all short of this one. Truncation, not an error: a large result is not
// a refusal the model needs to adapt its call to, just content it should
// know was cut.
void CapToolResult(llm::ToolResultBlock& block, std::int64_t max_bytes) {
    if (max_bytes <= 0) return;
    const auto cap = static_cast<std::size_t>(max_bytes);

    std::size_t total = 0;
    for (const auto& item : block.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&item)) total += text->text.size();
    }
    if (total <= cap) return;

    std::string kept;
    kept.reserve(cap);
    for (auto& item : block.content) {
        if (kept.size() >= cap) break;
        if (auto* text = std::get_if<llm::TextBlock>(&item)) {
            const auto take = std::min(cap - kept.size(), text->text.size());
            kept.append(text->text, 0, take);
        }
    }
    kept += "\n... [" + std::to_string(total - cap) +
            " bytes omitted; result exceeds the configured max_result_bytes (" +
            std::to_string(cap) + ")]";
    block.content.clear();
    block.content.push_back(llm::TextBlock{std::move(kept)});
}

// The display payload's ceiling (settings.json tools.max_display_bytes).
// DROPPED whole over the cap, never truncated: a cut diff or file excerpt
// misrepresents what happened, while the text fallback stays truthful.
// Returns the serialized size when it dropped the payload, 0 otherwise.
std::size_t CapToolDisplay(llm::ToolResultBlock& block, std::int64_t max_bytes) {
    if (max_bytes <= 0 || block.display.is_null()) return 0;
    const std::size_t size =
        block.display.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace).size();
    if (size <= static_cast<std::size_t>(max_bytes)) return 0;
    block.display = nullptr;
    return size;
}

// S4's no-boundary fallback: when snap_compaction_boundary finds no safe cut
// anywhere (every candidate lands inside an open tool-call cycle), context
// would otherwise grow forever -- but the actual harm that trips the trigger
// is usually SIZE, not message count, and a single oversized tool result is
// the common cause. Eliding one IN PLACE, reusing CapToolResult's own
// truncation shape, shrinks the context without removing any message, so it
// can never disturb tool_use/tool_result pairing -- the very thing that made
// a structural cut impossible in the first place. Returns false when nothing
// large enough to be worth eliding remains (every result is already small),
// which is the caller's signal to fall back to the genuine last resort.
//
// DIVERGENCE NOTE: unlike the structural path, elision mutates only the
// working history_ -- the persisted transcript keeps the full bytes (there is
// no message-rewrite API, and no summary to anchor a checkpoint to). A resume
// therefore regrows the elided content. That self-heals: the resume re-seeds
// occupancy from the regrown context, the trigger re-fires, and this path
// runs again -- the cost is one oversized request between resume and the next
// compaction cycle, not unbounded growth.
constexpr std::int64_t kElisionMinSizeToBother = 1000;  // below this, eliding buys nothing
constexpr std::int64_t kElisionTargetBytes = 500;

bool ElideLargestToolResult(std::vector<llm::Message>& history) {
    llm::ToolResultBlock* largest = nullptr;
    std::size_t largest_size = 0;
    for (auto& message : history) {
        for (auto& block : message.content) {
            auto* result = std::get_if<llm::ToolResultBlock>(&block);
            if (result == nullptr) continue;
            std::size_t size = 0;
            for (const auto& item : result->content) {
                if (const auto* text = std::get_if<llm::TextBlock>(&item)) size += text->text.size();
            }
            if (size > largest_size) {
                largest_size = size;
                largest = result;
            }
        }
    }
    if (largest == nullptr || static_cast<std::int64_t>(largest_size) <= kElisionMinSizeToBother) {
        return false;
    }
    CapToolResult(*largest, kElisionTargetBytes);
    largest->display = nullptr;   // it described the full result, not the stub
    return true;
}

}  // namespace

AgentLoop::AgentLoop(llm::LlmProvider& provider, tools::ToolExecutor& executor,
                      tools::ToolRegistry& registry, session::HistoryStore& store, AgentPreset preset,
                      LoopConfig config, SessionContext session,
                      std::function<bool()> is_cancelled, llm::StreamEventSink event_sink,
                      Compactor compactor, ResumeSeed seed, std::string project_context)
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
      history_(std::move(seed.history)),
      first_persisted_seq_(seed.first_persisted_seq),
      head_is_summary_(seed.head_is_summary),
      pool_(config.max_parallel_tool_calls) {
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
        // estimate with ground truth anyway. Covers the whole request context
        // (system prompt, project context, tools array), not just the history:
        // those ride every request and backend ground truth counts them, so
        // excluding them under-triggered compaction on exactly the sessions
        // this seed exists for.
        tokens_.SeedOccupancy(EstimateContextTokens());
    }
}

bool AgentLoop::Cancelled() const { return is_cancelled_ && is_cancelled_(); }

bool AgentLoop::SleepForBackoff(int attempt,
                                std::optional<std::chrono::milliseconds> retry_after) {
    if (config_.retry_base_delay.count() <= 0 && !retry_after) return !Cancelled();

    // Exponential with EQUAL jitter: half the ceiling fixed, half random.
    // Jitter matters even for one local user -- several sessions that failed
    // against the same backend would otherwise retry in lockstep forever -- but
    // full jitter (drawing from [0, ceiling]) can legally return zero, which
    // makes "the retry waited" unassertable in a single run and leaves a real
    // 429 answered immediately every so often. Half the ceiling is the floor.
    // `attempt` tracks retries for a configured max_retries that could exceed
    // 31; shifting that far is UB. Anything past 2^30 ms is already far beyond
    // retry_max_delay, so the min() below clamps it regardless.
    const auto ceiling =
        std::min(config_.retry_base_delay * (1 << std::min(attempt, 30)), config_.retry_max_delay);
    const std::int64_t half = ceiling.count() / 2;
    std::uniform_int_distribution<std::int64_t> pick(0, ceiling.count() - half);
    std::chrono::milliseconds delay{half + pick(jitter_)};

    // The server's own hint outranks the computed backoff: retrying a 429
    // sooner than its Retry-After converts one rate-limit event into
    // max_retries more. The hint was parsed, carried and tested end to end --
    // and then ignored here, the only consumer there ever was.
    if (retry_after && *retry_after > delay) delay = *retry_after;

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

// Unrelated to Session::QueueInjectedMessage despite the similar shape: that
// one guards on terminated_ and wakes the session's own worker via turn_cv_,
// neither of which AgentLoop has -- this is a bare push, used directly only
// by tests that construct a bare AgentLoop with no Session wrapper.
bool AgentLoop::PushInjectedMessage(llm::Message message) {
    return injections_->Push(std::move(message));
}

void AgentLoop::RepairOrphanedToolUses(
    const std::vector<const llm::ToolUseBlock*>& tool_uses) {
    if (tool_uses.empty()) return;
    // The assistant message was appended (and persisted) BEFORE the
    // stop-reason checks, so a turn that ends here -- a cancel landing after
    // the model finished streaming tool calls (the most natural cancel
    // moment), a MaxTokens cut with complete tool_calls, a refusal -- leaves
    // history ending in unanswered tool_use: a hard 400 on both wire formats,
    // on EVERY later turn. The live repair is the resume path's
    // (synthesize_missing_tool_results): one error ToolResultBlock per
    // unanswered call, appended and persisted as one real message. Cancellation
    // is ordinary, not exceptional -- the same every-exit-path rule that makes
    // a THROWING tool survivable (§12.2) and fires the completion hook on
    // teardown applies to the transcript itself.
    if (auto repair = synthesize_missing_tool_results(history_)) {
        AppendAndPersist(std::move(*repair));
    }
}

void AgentLoop::FlushInjectedMessages() {
    // DrainAll runs the sink outside the queue's mutex; AppendAndPersist
    // touches the store and must not hold it.
    injections_->DrainAll([this](llm::Message message) { AppendAndPersist(std::move(message)); });
}

AgentLoop::SystemAndTools AgentLoop::BuildSystemAndTools() const {
    SystemAndTools out;
    if (!preset_.system_prompt.empty()) out.system = preset_.system_prompt;
    // Project context appends AFTER the preset body: both are constant within
    // the session, so the request's prefix -- which the provider's prompt cache
    // keys on -- is stable either way, but appending keeps the preset's own
    // text at the very front across /agent switches.
    if (!project_context_.empty()) {
        if (out.system) *out.system += "\n\n";
        else out.system = std::string();
        *out.system += project_context_;
    }

    std::vector<std::string> allowed = preset_.allowed_tools;
    if (preset_.is_subagent) {
        // Hard rule, not preset config: a subagent never gets the spawn tool, so
        // a misconfigured preset cannot reopen recursion. Subagents are flat,
        // one level deep.
        allowed.erase(std::remove(allowed.begin(), allowed.end(), kSpawnSubagentToolName),
                      allowed.end());
    }

    out.tools = registry_.DefinitionsFor(allowed);
    // Live MCP tools for the allowed-and-alive servers, merged into the same
    // sorted order -- this request is rebuilt every turn, so the merge must
    // preserve the deterministic prefix the prompt cache keys on.
    {
        std::vector<llm::ToolDefinition> mcp_defs =
            registry_.DefinitionsForMcpServers(mcp_servers_);
        out.tools.insert(out.tools.end(), mcp_defs.begin(), mcp_defs.end());
        std::sort(out.tools.begin(), out.tools.end(),
                  [](const llm::ToolDefinition& a, const llm::ToolDefinition& b) {
                      return a.name < b.name;
                  });
    }
    if (preset_.is_subagent) {
        // DefinitionsFor("*") would have re-admitted it.
        out.tools.erase(std::remove_if(out.tools.begin(), out.tools.end(),
                                       [](const llm::ToolDefinition& def) {
                                           return def.name == kSpawnSubagentToolName;
                                       }),
                        out.tools.end());
    }
    if (preset_.allowed_skills.empty()) {
        // No skills in this preset: the tool would only ever return errors. One
        // post-erase suffices here (unlike the subagent case above) because it
        // covers both an explicit allowed_tools list and "*".
        out.tools.erase(std::remove_if(out.tools.begin(), out.tools.end(),
                                       [](const llm::ToolDefinition& def) {
                                           return def.name == kSkillToolName;
                                       }),
                        out.tools.end());
    }
    return out;
}

llm::ChatRequest AgentLoop::BuildChatRequest() const {
    llm::ChatRequest request;
    SystemAndTools system_and_tools = BuildSystemAndTools();
    request.system = std::move(system_and_tools.system);
    request.tools = std::move(system_and_tools.tools);
    request.messages = history_;
    request.model = preset_.provider.model_name;
    request.parameters = preset_.parameters;
    request.max_tokens = preset_.max_tokens;
    request.stream = true;
    return request;
}

// The estimate is built FROM the same system/tools construction BuildChatRequest
// uses -- system prompt, project context and tools array included -- rather
// than alongside it, so the two cannot drift: a /agent switch, a
// project-context load or an allowed-tools change flow through
// BuildSystemAndTools into both. Deliberately does NOT go through
// BuildChatRequest: that assigns request.messages = history_, a full deep copy
// of the working history's every message and string that this call never
// reads -- for a 512k-1M-token session, on every compaction check, resume, and
// rewind/redo. The bytes/4 roughness matches estimate_tokens, which is
// deliberate: the first real response replaces the estimate with the
// backend's ground truth anyway.
std::int64_t AgentLoop::EstimateContextTokens() const {
    const SystemAndTools system_and_tools = BuildSystemAndTools();
    return EstimateContextTokens(system_and_tools.system, system_and_tools.tools);
}

std::int64_t AgentLoop::EstimateContextTokens(const std::optional<std::string>& system,
                                              const std::vector<llm::ToolDefinition>& tools) const {
    std::int64_t bytes = system ? static_cast<std::int64_t>(system->size()) : 0;
    for (const auto& def : tools) {
        bytes += static_cast<std::int64_t>(def.name.size() + def.description.size());
        bytes += JsonSizeEstimate(def.input_schema);
    }
    return bytes / 4 + estimate_tokens(history_);
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
        // to be contiguous with, and the next append establishes it. The
        // summary flag goes with it: an empty set has no synthetic head, and
        // leaving it set would make the next Compact() subtract a phantom
        // summary in persisted_removed.
        first_persisted_seq_.reset();
        head_is_summary_ = false;
    }

    // Same reset the constructor and /clear apply: the estimate described the
    // discarded tail too.
    tokens_ = llm::TokenAccumulator{};
    if (!history_.empty()) tokens_.SeedOccupancy(EstimateContextTokens());

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
    if (!history_.empty()) tokens_.SeedOccupancy(EstimateContextTokens());
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
    tokens_.SeedOccupancy(EstimateContextTokens());
    session_.logger.info("session {}: redo restored {} messages from seq {}",
                         session_.session_id, history_.size(), first_seq);
}

std::optional<std::size_t> AgentLoop::Compact(std::string_view trigger) {
    if (!compactor_) return std::nullopt;
    if (history_.size() < 2) return std::nullopt;
    if (compaction_failed_this_turn_) return std::nullopt;

    // Keep the tail verbatim; summarize everything older. The candidate cut
    // comes from the configured strategy's cut selector (S4) when one is
    // set -- honoring memory.compaction_keep_recent_tokens (W11) via a
    // token-budget walk from the tail, instead of the legacy plain halving
    // -- else the boundary snap below is what makes ANY candidate safe.
    std::size_t candidate = config_.compaction_strategy.cut_selector
                                ? config_.compaction_strategy.cut_selector(
                                      history_, config_.compaction_keep_recent_tokens)
                                : history_.size() / 2;

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
            session_.session_id, history_.size());
        compaction_failed_this_turn_ = true;  // still don't retry every tool cycle
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
    const std::size_t cut = snap_compaction_boundary(history_, candidate);
    if (cut == 0 || cut >= history_.size()) {
        // No safe structural cut. Rather than accept unbounded growth, try to
        // shrink the context directly: elide the largest tool result in
        // place (see ElideLargestToolResult) -- this never disturbs message
        // boundaries, so it cannot make the pairing problem worse, and it
        // addresses the actual harm (context SIZE) even though it does not
        // fold any messages together.
        if (ElideLargestToolResult(history_)) {
            tokens_.SeedOccupancy(EstimateContextTokens());
            session_.logger.warn(
                "session {}: no safe compaction boundary in {} messages; elided the largest "
                "tool result instead",
                session_.session_id, history_.size());
            compaction_failed_this_turn_ = true;   // still don't retry every tool cycle
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

    // small_model, when configured: the summarizer ALONE runs on the alternate
    // provider -- its model name, parameters and cap; the session's own model
    // still answers every turn. A POINTER, not a reference: re-seating a
    // reference is impossible, and an assignment through one would run the
    // small provider's copy-assignment against the session's own provider --
    // corrupting it for every later turn -- rather than switching which
    // provider the summarizer call uses.
    llm::LlmProvider* summarizer = &provider_.get();
    if (config_.small_model.has_value() && config_.small_model->provider != nullptr) {
        summarizer = config_.small_model->provider.get();
        params.model_name = config_.small_model->model_name;
        params.parameters = config_.small_model->parameters;
        params.max_tokens = config_.small_model->max_tokens;
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
                                                {"history_messages", history_.size()},
                                                {"occupancy_tokens", tokens_.last_occupancy()}});
    }

    const auto result = compactor_(to_compact, *summarizer, params);
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
    }
    // The summary is now the head WHATEVER the persisted bookkeeping did:
    // a loop constructed without a first-persisted seq (the direct-API case;
    // every Session seeding path sets one) still folds the summary into
    // history_[0], and leaving head_is_summary_ false would make every later
    // seq mapping -- and CheckpointBoundary() -- mis-count the synthetic head.
    head_is_summary_ = true;

    // Rebuilt with moves, not copies: the kept tail is shifted once (moving
    // strings, which is pointer swaps) rather than duplicated. The summary
    // message lands at the head, replacing everything the cut removed.
    std::vector<llm::Message> rebuilt;
    rebuilt.reserve(history_.size() - cut + 1);
    rebuilt.push_back(std::move(result->summary_message));
    std::move(history_.begin() + static_cast<long>(cut), history_.end(),
              std::back_inserter(rebuilt));
    history_.swap(rebuilt);

    // RE-SEED the occupancy estimate: the one site that never did. The fold
    // just halved the working set, but the accumulator still described the
    // pre-fold context -- so the next MaybeCompact (the same turn's next tool
    // cycle, or the next turn before any response reported usage) fired on a
    // number that no longer existed. A manual /compact was guaranteed one
    // spurious second summarization; an auto one could chain. SeedOccupancy
    // touches only the last-turn estimate, so the backend-reported counts the
    // accumulator carries stay intact.
    tokens_.SeedOccupancy(EstimateContextTokens());

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
                if (!SleepForBackoff(attempt, error.retry_after())) {
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
        // message is actually in history_ and the estimate can cover it.
        if (!result.usage_estimated) tokens_.RecordTurn(response.usage);
        result.stop_reason = response.stop_reason;

        // MOVED, not copied. `response` is loop-local and dead after this point
        // -- the next iteration reassigns it -- and content can be the whole
        // assistant turn, so this was a full duplicate of every block and string
        // on the hottest path there is. The tool_uses pointers below stay valid
        // across the later move into history_: a vector move relocates the
        // heap buffer's ownership, not the elements.
        llm::Message assistant{llm::Role::Assistant, std::move(response.content)};
        result.final_text = llm::TextOf(assistant);
        const auto tool_uses = ToolUsesOf(assistant);
        // The only per-message token count that exists: this response's output.
        AppendAndPersist(std::move(assistant), response.usage.output_tokens);

        if (result.usage_estimated) {
            // Zero-usage proxy gap: the request is fully assembled now
            // (history_ carries this round's assistant message), so the same
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
            // DefinitionsFor/DefinitionsForMcpServers materialization and sort
            // on every zero-usage round, which is every round against any
            // OpenAI-compatible proxy that does not report usage.
            const std::int64_t estimate =
                EstimateContextTokens(chat_request.system, chat_request.tools);
            tokens_.SeedOccupancy(estimate);
            result.last_usage.input_tokens =
                static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(estimate, 0),
                                                        std::numeric_limits<int>::max()));
        }

        if (response.stop_reason == llm::StopReason::Cancelled) {
            RepairOrphanedToolUses(tool_uses);
            break;
        }

        // Anything other than ToolUse ends the turn. MaxTokens in particular is
        // reported as itself rather than collapsed into EndTurn -- otherwise
        // output is truncated mid-sentence and the turn looks successful.
        if (response.stop_reason != llm::StopReason::ToolUse) {
            RepairOrphanedToolUses(tool_uses);
            break;
        }

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
                                  ctx.ask_user = session_.ask_user;
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
            if (config_.max_tool_input_bytes > 0) {
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
            }
            // Unknown tools arrive here with no resolved tool; they execute as
            // their denial result and have no exclusivity to protect.
            call.to_pool = !denied_for_input && prepared.tool &&
                           prepared.tool->parallel_safe();
            call.prepared = std::move(prepared);
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
            CapToolResult(tool_result, config_.max_tool_result_bytes);
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

        try {
            AppendAndPersist(llm::Message{llm::Role::User, std::move(results)});
        } catch (...) {
            // The assistant message carrying these tool_use blocks was
            // already appended (and persisted) above. If persisting the
            // paired results now fails (a store fault -- locked/full/corrupt
            // database), history_ is left ending in unanswered tool_use: a
            // hard 400 on both wire formats, on EVERY later turn, forever --
            // this exception propagates past RunTurnLoop uncaught, and
            // nothing else repairs the in-memory working set (only resume's
            // LoadResumeSeed does, which requires a process restart).
            //
            // Best-effort in-memory repair, mirroring what the resume path
            // already does for the identical shape (synthesize_missing_
            // tool_results): synthesize error results so a live session
            // survives to its NEXT turn instead of being poisoned by this
            // one's store failure for the rest of its life. Guarded
            // separately: RepairOrphanedToolUses itself calls
            // AppendAndPersist, which can throw again if the store is still
            // down -- that must not replace the ORIGINAL exception (the
            // caller needs to know the store failed, not that the repair
            // attempt also failed), and if the store call happens to
            // succeed this time, the repair genuinely lands.
            try {
                RepairOrphanedToolUses(tool_uses);
            } catch (...) {
                // The store is what's failing; nothing more to do in memory.
            }
            throw;
        }

        // Turn boundary: safe to splice in anything a background subagent
        // finished while this turn was running.
        FlushInjectedMessages();
    }

    FlushInjectedMessages();
}

}  // namespace minimal::agent
