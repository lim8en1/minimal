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
                      Compactor compactor)
    : provider_(provider),
      executor_(executor),
      registry_(registry),
      store_(store),
      preset_(std::move(preset)),
      config_(config),
      session_(std::move(session)),
      is_cancelled_(std::move(is_cancelled)),
      event_sink_(std::move(event_sink)),
      compactor_(std::move(compactor)) {}

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
    history_.push_back(std::move(message));
}

void AgentLoop::QueueInjectedMessage(llm::Message message) {
    injected_.push_back(std::move(message));
}

void AgentLoop::FlushInjectedMessages() {
    if (injected_.empty()) return;
    std::vector<llm::Message> pending;
    pending.swap(injected_);
    for (auto& message : pending) {
        AppendAndPersist(std::move(message));
    }
}

llm::ChatRequest AgentLoop::BuildChatRequest() const {
    llm::ChatRequest request;
    if (!preset_.system_prompt.empty()) request.system = preset_.system_prompt;
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
    Compact();
}

std::optional<std::size_t> AgentLoop::CompactNow() {
    // The TRIGGER is skipped -- the user asked, so occupancy, auto_compact and
    // the configured strategy are all beside the point. Everything Compact()
    // checks is not: an empty history, a missing compactor and a summarizer that
    // already failed this turn are just as true when the request is manual.
    return Compact();
}

void AgentLoop::ClearHistory() {
    history_.clear();
    injected_.clear();
    // Not the store. Compaction established that the in-memory working set and
    // the durable transcript are different things; /clear drops the first and
    // leaves scrollback whole.
}

std::optional<std::size_t> AgentLoop::Compact() {
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
        return std::nullopt;
    }

    const std::vector<llm::Message> to_compact(history_.begin(), history_.begin() + cut);
    CompactionRequest params;
    params.model_name = preset_.provider.model_name;
    params.compression_prompt = preset_.compression_prompt;
    params.parameters = preset_.parameters;
    params.max_tokens = preset_.max_tokens;
    params.last_reported_input_tokens = tokens_.last_input_tokens();
    params.full_history = &history_;
    params.is_cancelled = is_cancelled_;

    const auto result = compactor_(to_compact, provider_.get(), params);
    if (!result) {
        // No compaction this cycle; do not retry on every tool cycle of this turn.
        compaction_failed_this_turn_ = true;
        return std::nullopt;
    }

    std::vector<llm::Message> rebuilt;
    rebuilt.reserve(history_.size() - cut + 1);
    rebuilt.push_back(std::move(result->summary_message));
    rebuilt.insert(rebuilt.end(), history_.begin() + cut, history_.end());
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

    // NO COOLDOWN ON SUCCESS, deliberately. Repeated compaction inside one turn
    // looks unbounded but is not: each pass replaces the first `cut` ~= size/2
    // messages with one, so history halves every time and `history_.size() < 2`
    // is the floor -- it terminates in log2(n) passes. Across turns, compacting
    // again once new messages have pushed occupancy back over the trigger is the
    // intended behaviour, not a runaway.

    // Deliberately not persisted: compaction changes only what the NEXT request
    // contains. The store keeps the full uncompacted record, so scrollback is
    // unaffected no matter how many times this runs.
    return cut;
}

TurnResult AgentLoop::RunTurn(llm::Message seed_message) {
    TurnResult result;
    compaction_failed_this_turn_ = false;
    AppendAndPersist(std::move(seed_message));

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

        llm::ChatResponse response;
        bool got_response = false;
        for (int attempt = 0; attempt <= config_.max_retries; ++attempt) {
            try {
                response = provider_.get().StreamComplete(BuildChatRequest(), event_sink_);
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
                if (!SleepForBackoff(attempt)) break;
            }
        }
        if (!got_response) break;

        ++result.provider_calls;
        result.last_usage = response.usage;
        tokens_.RecordTurn(response.usage);
        result.stop_reason = response.stop_reason;

        llm::Message assistant{llm::Role::Assistant, response.content};
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

        // Sequential in v1: no per-session thread pool, permission prompts
        // arrive one at a time, and no ordering rules are needed for interleaved
        // tool events.
        std::vector<llm::ContentBlock> results;
        results.reserve(tool_uses.size());
        for (const auto* use : tool_uses) {
            tools::ToolContext ctx{session_.logger};
            ctx.session_id = session_.session_id;
            ctx.project_root = session_.project_root;
            ctx.allowed_skills = preset_.allowed_skills;
            ctx.is_cancelled = is_cancelled_;
            if (session_.tool_output_sink) {
                const std::string tool_use_id = use->id;
                ctx.emit_output = [this, tool_use_id](std::string_view chunk) {
                    session_.tool_output_sink(tool_use_id, chunk);
                };
            }

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

            llm::ToolResultBlock tool_result = executor_.Run(use->id, use->name, use->input, ctx);

            // Emitted even when Run() reports an error, and before the result is
            // appended: a client that showed a tool as started must always see it
            // finish, or its running-tools panel accumulates entries that never
            // clear.
            lifecycle.started = false;
            lifecycle.is_error = tool_result.is_error;
            if (session_.tool_lifecycle_sink) session_.tool_lifecycle_sink(lifecycle);

            results.push_back(std::move(tool_result));
        }
        AppendAndPersist(llm::Message{llm::Role::User, std::move(results)});

        // Turn boundary: safe to splice in anything a background subagent
        // finished while this turn was running.
        FlushInjectedMessages();
    }

    FlushInjectedMessages();
    return result;
}

}  // namespace minimal::agent
