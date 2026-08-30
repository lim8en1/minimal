#include "minimal/agent/agent_loop.hpp"

#include <algorithm>
#include <utility>
#include <variant>

#include "minimal/agent/compaction_boundary.hpp"

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

void AgentLoop::AppendAndPersist(llm::Message message) {
    store_.Append(message);
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
    if (config_.compaction_trigger_tokens <= 0) return;
    if (tokens_.last_occupancy() < config_.compaction_trigger_tokens) return;
    if (!compactor_) return;
    if (history_.size() < 2) return;
    if (compaction_failed_this_turn_) return;

    // Keep the tail verbatim; summarize everything older. The candidate cut is
    // chosen by count here -- the boundary snap is what makes it safe.
    std::size_t candidate = history_.size() / 2;

    // Snapping backward to a complete tool-call cycle is mandatory: cutting
    // between an assistant ToolUseBlock and its ToolResultBlock orphans the call,
    // which is a hard 400 on both OpenAI and Anthropic rather than a soft
    // degradation.
    const std::size_t cut = snap_compaction_boundary(history_, candidate);
    if (cut == 0 || cut >= history_.size()) return;

    const std::vector<llm::Message> to_compact(history_.begin(), history_.begin() + cut);
    const auto result = compactor_(to_compact, provider_, preset_.provider.model_name,
                                   preset_.compression_prompt, tokens_.last_input_tokens(),
                                   is_cancelled_);
    if (!result) {
        // No compaction this cycle; do not retry on every tool cycle of this turn.
        compaction_failed_this_turn_ = true;
        return;
    }

    std::vector<llm::Message> rebuilt;
    rebuilt.reserve(history_.size() - cut + 1);
    rebuilt.push_back(std::move(result->summary_message));
    rebuilt.insert(rebuilt.end(), history_.begin() + cut, history_.end());
    history_.swap(rebuilt);

    // Deliberately not persisted: compaction changes only what the NEXT request
    // contains. The store keeps the full uncompacted record, so scrollback is
    // unaffected no matter how many times this runs.
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
                response = provider_.StreamComplete(BuildChatRequest(), event_sink_);
                got_response = true;
                break;
            } catch (const llm::LlmError& error) {
                // Retry policy lives here, not in the provider: only the loop
                // knows whether another attempt is still wanted.
                if (!error.is_retryable() || attempt == config_.max_retries) {
                    throw;
                }
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
        AppendAndPersist(std::move(assistant));

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
            results.push_back(executor_.Run(use->id, use->name, use->input, ctx));
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
