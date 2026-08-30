#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/history_store.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/stream_event.hpp"
#include "minimal/llm/token_accounting.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::agent {

struct LoopConfig {
    bool auto_compact = true;
    std::int64_t compaction_trigger_tokens = 0;      // 0 = never
    std::int64_t compaction_keep_recent_tokens = 0;
    int max_retries = 3;
    int max_turns = 0;  // 0 = unbounded; subagents bound their own life
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

    // Handed to every ToolContext this session builds. Session-scoped rather than
    // a free function so a later per-session sink (session-id prefix) is an
    // implementation detail of the wiring, not of the tools.
    common::Logger& logger;
};

struct TurnResult {
    llm::StopReason stop_reason = llm::StopReason::Unknown;
    // Final assistant text. A subagent's whole result is this string, handed
    // back to the parent as its ToolResultBlock.
    std::string final_text;
    llm::Usage last_usage;
    int provider_calls = 0;
    bool hit_max_turns = false;
};

// Summarizes a message range into one synthetic message. Injected so the
// compaction path is testable without a live summarizer; production supplies one
// that calls the provider with prompts/compression.md.
using Compactor = std::function<llm::Message(const std::vector<llm::Message>&)>;

// Runs on the session worker thread. Blocks freely -- permission waits, provider
// calls, tool subprocesses -- which is safe only because the gRPC reader thread
// is separate and keeps draining.
class AgentLoop {
public:
    AgentLoop(llm::LlmProvider& provider, tools::ToolExecutor& executor,
              tools::ToolRegistry& registry, HistoryStore& store, AgentPreset preset,
              LoopConfig config, SessionContext session, std::function<bool()> is_cancelled,
              llm::StreamEventSink event_sink, Compactor compactor);

    // Appends `seed_message`, then runs to a natural stop. A subagent's entire
    // life is one call to this -- there is one loop implementation, not two.
    TurnResult RunTurn(llm::Message seed_message);

    // Queued by a non-blocking subagent's completion, flushed at a turn boundary
    // rather than injected out-of-band: landing a user message between a
    // tool_use and its tool_result is a hard 400 on both wire formats.
    void QueueInjectedMessage(llm::Message message);

    const std::vector<llm::Message>& history() const { return history_; }
    llm::ChatRequest BuildChatRequest() const;

private:
    void AppendAndPersist(llm::Message message);
    void MaybeCompact();
    void FlushInjectedMessages();
    bool Cancelled() const;

    llm::LlmProvider& provider_;
    tools::ToolExecutor& executor_;
    tools::ToolRegistry& registry_;
    HistoryStore& store_;
    AgentPreset preset_;
    LoopConfig config_;
    SessionContext session_;
    std::function<bool()> is_cancelled_;
    llm::StreamEventSink event_sink_;
    Compactor compactor_;

    std::vector<llm::Message> history_;
    std::vector<llm::Message> injected_;
    llm::TokenAccumulator tokens_;
};

}  // namespace minimal::agent
