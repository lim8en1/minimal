#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/agent/agent_preset.hpp"
#include "minimal/agent/context_compactor.hpp"
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

    // Exponential-with-full-jitter backoff between retries. Zero disables the
    // wait, which is what the tests that assert on attempt COUNTS want -- they
    // would otherwise pay the delay for nothing.
    std::chrono::milliseconds retry_base_delay{500};
    std::chrono::milliseconds retry_max_delay{8000};
};

// Brackets one tool invocation, surfaced to the client as ToolExecutionEvent.
// Distinct from a permission request, which BLOCKS and needs an answer: this is
// a non-gated status push, so a client can show "Running bash..." for a call
// that was never gated at all.
//
// Carries no summary line. Producing one means asking the tool to Disclose(),
// which for bash resolves argv[0] against the session PATH -- worth paying when
// a human is about to be asked a question, not on every call of an
// already-allowed tool. Rendering from tool_name + input is the client's job.
struct ToolLifecycleEvent {
    std::string tool_use_id;
    std::string tool_name;
    nlohmann::json input;
    bool started = false;
    bool is_error = false;  // meaningful only when started == false
    std::int64_t started_at_unix_ms = 0;
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

    // Start/finish brackets for each tool call. Optional: unset in tests that
    // only care about the transcript.
    std::function<void(const ToolLifecycleEvent&)> tool_lifecycle_sink;

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
// compaction path is testable without a live summarizer; production supplies
// compact_messages, which calls the provider with the preset's compression prompt.
// nullopt means "no compaction this cycle" -- failure degrades to no compaction,
// never to a lost transcript. Everything that varies is in CompactionRequest,
// supplied per call rather than bound at construction, because /agent can switch
// presets mid-session and binding would compact with the previous one's settings.
using Compactor = std::function<std::optional<CompactionResult>(
    const std::vector<llm::Message>& messages_to_compact, llm::LlmProvider& provider,
    const CompactionRequest& request)>;

// Runs on the session worker thread. Blocks freely -- permission waits, provider
// calls, tool subprocesses -- which is safe only because the gRPC reader thread
// is separate and keeps draining.
class AgentLoop {
public:
    AgentLoop(llm::LlmProvider& provider, tools::ToolExecutor& executor,
              tools::ToolRegistry& registry, HistoryStore& store, AgentPreset preset,
              LoopConfig config, SessionContext session, std::function<bool()> is_cancelled,
              llm::StreamEventSink event_sink, Compactor compactor,
              // RESUME: the stored transcript, loaded without being re-appended.
              // Construction-time seeding (rather than a later setter) means a
              // turn can never run against a half-initialized history. Empty for
              // a new session. Seeded history also seeds the occupancy estimate,
              // so auto-compaction can fire BEFORE the first post-resume
              // provider call -- without that, a resumed session whose stored
              // context exceeds the window would fail the call outright.
              std::vector<llm::Message> seeded_history = {});

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
    void AppendAndPersist(llm::Message message, std::optional<int> output_tokens = std::nullopt);
    void MaybeCompact();
    void FlushInjectedMessages();
    bool Cancelled() const;
    // Waits before retry `attempt`, polling cancellation. False means "give up"
    // -- cancelled during the wait -- so the caller stops retrying.
    bool SleepForBackoff(int attempt);

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
    // Backoff jitter only. Loop-owned rather than shared, so two sessions
    // retrying the same failing backend do not draw the same delays.
    std::mt19937 jitter_{std::random_device{}()};

    // Set when a compaction attempt fails, cleared at each RunTurn: without it a
    // failing summarizer would be re-invoked on every tool cycle of the same turn,
    // resending half the context to the provider each time. The trigger stays armed
    // for the next turn, where the flag resets.
    bool compaction_failed_this_turn_ = false;
};

}  // namespace minimal::agent
