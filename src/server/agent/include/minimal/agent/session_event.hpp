#pragma once

// The domain vocabulary Session emits on its event sink. rpc::ToProto
// translates each alternative to v1::ServerEvent's oneof -- this header must
// never include anything from rpc/ or any generated proto header, or the
// inversion the type exists for is undone at the include level.
//
// EVERY PAYLOAD IS AN OWNED VALUE, never a reference -- same discipline as
// TurnSummary/ToolInvocation in session_observer.hpp, and for the same
// reason: a sink may run the translation on a thread that outlives the call
// that produced the event.

#include <cstdint>
#include <functional>
#include <string>
#include <variant>

#include "minimal/agent/agent_loop.hpp"
#include "minimal/commands/command.hpp"
#include "minimal/llm/chat.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/stream_event.hpp"
#include "minimal/tools/tool.hpp"

namespace minimal::agent {

// Live (tail-only) tool stdout. Distinct from ToolLifecycleEvent's
// start/finish brackets -- this is the output in between.
struct ToolOutputDelta {
    std::string tool_use_id;
    std::string chunk;
};

// commands::CommandResult alone cannot cross to the wire: the wire's
// CommandResult.request_id correlates back to the client's
// CommandInvocation, but that id lives on QueuedCommand, not on the outcome
// a Command returns. This pairs them back together for the one place that
// needs both.
struct SessionCommandResult {
    std::string request_id;
    commands::CommandResult result;
};

// Emitted on the PARENT session when a subagent is spawned. Must be emitted
// before the child produces its first event -- attaching to the child is a
// client protocol obligation (ARCHITECTURE.md "Session lifetime").
struct SubagentStarted {
    std::string child_session_id;
    std::string task;
    bool blocking = false;
};

// Session metadata changed server-side (name, or the full E-additions set at
// attach time). `partial`: true means only `name` is meaningful -- the other
// fields are the zero value and must not overwrite what the client already
// has for them.
struct SessionMetadataChanged {
    std::string name;
    std::string agent_preset;
    std::string model;
    std::string permission_mode;
    std::int64_t context_window_tokens = 0;
    bool partial = false;
};

// The session was terminated server-side. Must be emitted before the
// transport queue closes, on every terminate path.
struct SessionEnded {};

// The transcript was rewound (truncated_to_seq >= 0) or redone
// (truncated_to_seq == -1). active_last_seq mirrors SessionRewoundEvent.
struct SessionRewound {
    std::int64_t truncated_to_seq = 0;
    std::int64_t active_last_seq = 0;
};

// One turn reached a natural stop. Deliberately NOT agent::TurnSummary:
// TurnSummary is the plugin-facing SessionObserver payload (first_seq,
// tool_names_invoked, ...) assembled separately over the same TurnResult --
// this is the wire's narrower shape (mirrors TurnCompletedEvent exactly).
struct TurnCompleted {
    llm::StopReason stop_reason = llm::StopReason::Unknown;
    llm::Usage usage;
    bool hit_max_turns = false;
};

// The ad-hoc refusal/error sites (backlog full, malformed rewind, hook veto,
// MCP server disabled, ...) that today all hardcode
// category=LLM_ERROR_CATEGORY_UNSPECIFIED / no http_status / is_retryable=
// false -- only `message` ever varies. A real provider error is llm::LlmError
// (below), never this: SessionError exists so these sites don't have to
// misuse LlmError's exception-typed constructor for something that isn't a
// provider error.
struct SessionError {
    std::string message;
};

using SessionEvent = std::variant<llm::StreamEvent, ToolLifecycleEvent, tools::BackgroundTaskEvent,
                                  llm::LlmError, SessionCommandResult, ToolOutputDelta,
                                  SubagentStarted, SessionMetadataChanged, SessionEnded,
                                  SessionRewound, TurnCompleted, SessionError>;

// Returns whether the transport accepted the event -- mirrors
// EventBroadcaster::Publish's own return exactly (false only means the
// broadcaster is closed, i.e. the session is terminating). Not void: several
// call sites warn on a dropped event, and the stream event path uses the
// return value to abort an in-flight provider request when a race loses to
// Terminate between the IsCancelled() check and the publish.
using SessionEventSink = std::function<bool(const SessionEvent&)>;

}  // namespace minimal::agent
