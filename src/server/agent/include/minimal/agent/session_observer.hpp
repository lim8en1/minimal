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
    // True when `usage` is the request's bytes/4 estimate rather than a
    // backend-reported count (the zero-usage proxy gap). A pricing observer
    // must treat an estimated turn as unreported -- an estimated token count
    // is not a reported one.
    bool usage_estimated = false;
    // The turn's CUMULATIVE usage: the sum across every provider round of the
    // turn (each round is a separately-billed request, so summing is what
    // cost accounting needs -- `usage` above is deliberately the LAST round's
    // occupancy-shaped count). Rounds whose usage was estimated (unreported)
    // contribute nothing to the sum.
    llm::Usage turn_usage;
    // False when ANY round of the turn went unreported (estimated): the sum
    // above is then a lower bound, and a pricing observer must treat the turn
    // as unpriced rather than bill a partial count as if it were whole.
    bool turn_usage_fully_reported = true;
};

// One provider round answered inside a still-running turn (AgentLoop fires it
// the moment a response lands, NOT at turn end). The sanctioned channel for
// usage that must be visible MID-turn -- a cost tracker showing a running
// total while tools run, including while subagents run -- without waiting for
// OnTurnCompleted's turn-granular summary.
struct RoundUsage {
    // The request's model (the same name TurnSummary::model carries, so a
    // per-model rate resolves identically for rounds and turns).
    std::string model;
    // This round's usage only -- one provider request, never cumulative.
    llm::Usage usage;
    // True when `usage` is the request's bytes/4 estimate rather than a
    // backend-reported count (the zero-usage proxy gap). Same rule as
    // TurnSummary::usage_estimated: an estimated count is not a reported one,
    // so a pricing consumer must record the round's EXISTENCE but not its
    // tokens.
    bool usage_estimated = false;
};

struct ToolInvocation {
    std::string tool_use_id;
    std::string tool_name;
    std::string summary;
    bool is_error = false;
    std::chrono::milliseconds duration{0};
    // The same labels core.pre_tool_use carries (ToolExecutor::hook_labels):
    // the permission key (never an alias) and the input-sensitive category
    // ("read_only" | "file_write" | "execute" | "mcp"). Appended last so the
    // layout prefix a plugin was built against is unchanged.
    std::string permission_key;
    std::string category;
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
    // Per provider round inside a live turn (see RoundUsage). Fires BETWEEN
    // OnSessionCreated and OnTurnCompleted for the same session, on the same
    // ordered dispatch thread -- ordering an observer can rely on.
    virtual void OnProviderRound(const SessionInfo&, const RoundUsage&) {}
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
