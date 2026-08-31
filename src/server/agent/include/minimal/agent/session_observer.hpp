#pragma once

// The one extension point for REACTING to harness events, as opposed to
// providing an implementation of something. Session naming, turn-completion
// notifications, git auto-commit, usage tracking, external audit sinks.
//
// OnSessionEnded is not optional garnish. Any plugin holding per-session state
// leaks it for the process lifetime without one -- tools-bash holding a cwd/env
// map is the concrete case, since sessions terminate and nothing else would tell
// the plugin. See ARCHITECTURE.md "SessionObserver -- observe and react".

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "minimal/llm/chat.hpp"
#include "minimal/llm/message.hpp"

namespace minimal::agent {

struct SessionInfo {
    std::string session_id;
    std::string parent_session_id;  // empty for a top-level session
    std::string name;
    std::string agent_preset;
    std::filesystem::path project_root;
};

struct TurnSummary {
    std::int64_t first_seq = 0;
    std::int64_t last_seq = 0;
    llm::StopReason stop_reason = llm::StopReason::Unknown;
    llm::Usage usage;
    std::chrono::milliseconds duration{0};
    std::vector<std::string> tool_names_invoked;
    // The model the turn's provider calls were made with (the request's model).
    // A usage-tracking observer cannot price a turn without it: /model can
    // switch mid-session, so the preset or provider entry alone underdetermines
    // the rate.
    std::string model;
};

struct ToolInvocation {
    std::string tool_use_id;
    std::string tool_name;
    std::string summary;
    bool is_error = false;
    std::chrono::milliseconds duration{0};
};

// EVERY PAYLOAD IS AN OWNED VALUE, never a reference. Observers run on a
// detached task, so a `const std::vector<Message>&` would dangle. That is also
// why the payloads are deliberately small: rather than copying turn content for
// every observer that does not want it, an observer that needs content will
// query by seq range through SessionReader (which lands with SQLite storage).
//
// HARD CONSTRAINT: observers must never wedge a turn. They return void and
// cannot veto -- vetoing is PermissionChecker's job -- and they are dispatched
// fire-and-forget, never awaited.
class SessionObserver {
public:
    virtual ~SessionObserver() = default;
    virtual void OnSessionCreated(const SessionInfo&) {}
    virtual void OnTurnCompleted(const SessionInfo&, const TurnSummary&) {}
    virtual void OnToolExecuted(const SessionInfo&, const ToolInvocation&) {}
    // The session's transcript was rewound (truncated_to_seq >= 0: the cut
    // landed before that seq) or redone (-1: the discarded tail came back).
    // This is the seam a file-checkpointing plugin keys on: it maps its own
    // file snapshots to tool-invocation seqs (via OnToolExecuted) and restores
    // the snapshot at the cut when notified. Core never restores files.
    virtual void OnSessionRewound(const SessionInfo&, std::int64_t /*truncated_to_seq*/) {}
    virtual void OnSessionEnded(const SessionInfo&) {}
};

}  // namespace minimal::agent
