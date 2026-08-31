#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minimal/v1/common.pb.h"
#include "minimal/v1/session_service.pb.h"
#include "minimal/client/session_client.hpp"

namespace minimal::client {

// ============================================================================
// Tool execution tracking (for the running-tools panel)
//   Server emits: ToolExecutionEvent{started=true} -> ToolOutputDeltaEvents ->
//   ToolExecutionEvent{started=false, is_error}. We mirror this sequence.
// v1 runs tools sequentially, so there is at most one active tool per turn; each
// delta/finish event carries the tool_use_id matching its start — we key on that.
// ============================================================================

struct ToolExecutionState {
    std::string tool_use_id;
    std::string tool_name;
    std::optional<std::string> summary;      // human-readable from ToolExecutionEvent
    std::chrono::milliseconds started_at_unix_ms{-1};  // server-supplied timestamp

    // Accumulated chunks arrive here (lossy ring buffer). The final authoritative
    // record is the persisted ToolResultBlock, but this is what the panel shows.
    std::string buffered_output;

    bool started = false;      // true once we've seen a start event for this tool
    bool is_error = false;     // set by the finished ToolExecutionEvent (is_error)
};

// ============================================================================
// Pending interactions awaiting user input (permission/confirmation modals)
// ============================================================================

struct PendingPermission {
    std::string request_id;
    std::string tool_name;
    v1::ToolCategory category;   // READ_ONLY / FILE_WRITE / EXECUTE (UI tint)
    std::string summary;         // one-line description shown in the prompt
    bool complete = false;       // server offered full disclosure (false = lower bound)

    // Grant offers to present as session-grant buttons ("/usr/bin/make *"). Blanket
    // permission for the whole binary, any args. Worded exactly that way.
    std::vector<v1::GrantOffer> grants;
};

struct PendingConfirmation {
    std::string request_id;
    std::string kind;    // "tool_permission" | "command_confirmation"
    std::string title;
    std::string detail;
    std::vector<std::string> options;
};

// ============================================================================
// Session state kinds (derived from wire events, per ARCHITECTURE.md)
// ============================================================================

enum class SessionStateKind {
    Idle,        // ready for input
    Thinking,   // assistant text streaming in (TextDelta received)
    Working,    // one or more tool executions pending completion
    Waiting,    // permission/confirmation blocking the turn - attention glyph !
    Done,       // TurnCompletedEvent received without an error
    Error      // ErrorEvent received on the stream
};

// ============================================================================
// Per-session state (one SessionState per UI tab). Thread-safe; updated by the
// reader callback and read by a snapshot every frame.
// ============================================================================

class SessionState {
public:
    explicit SessionState(std::string session_id, std::string name = "");

    // Renderable copy taken under a short lock held minimally (no blocking I/O).
    struct Snapshot {
        std::string session_id;
        std::string display_name;
        SessionStateKind state = SessionStateKind::Idle;

        // Scrollback window (keyset-paged). Most recent N messages, descending.
        std::vector<v1::StoredMessage> scrollback_page;
        bool has_earlier_messages = false;   // paging back will yield more
        int64_t last_loaded_seq = -1;        // cursor for the next "load older"

        // Streaming assistant response (partial markdown building up)
        std::string streaming_text;          // empty when not streaming
        bool is_streaming_finalized = false;

        // Pending modals
        std::optional<PendingPermission> pending_permission;
        std::optional<PendingConfirmation> pending_confirmation;

        // Active tools in order emitted (server runs them sequentially in v1)
        std::vector<ToolExecutionState> tools;

        // Status-bar occupancy from the LAST turn only (NOT cumulative — see ARCH).
        int64_t input_tokens = 0;
        int64_t output_tokens = 0;
    };

    Snapshot TakeSnapshot();

    // --- Session lifecycle ---
    void OnSessionCreated(std::string name);
    void OnSessionMetadata(const v1::SessionMetadataEvent& metadata);

    // --- Streaming response ---
    void AppendStreamingText(std::string_view text);
    void FinalizeStreamingText();

    // --- Turn boundaries (ToolExecution / TextDelta set Working/Thinking; these clear it) ---
    void OnTurnCompleted(const v1::TurnCompletedEvent& evt);
    void OnError(const std::string& error_message);

    // The session's stream ended (server closed it / session terminated) --
    // the real "Done" (✓) signal, distinct from a turn merely completing.
    void OnSessionEnded();

    // --- Pending interactions ---
    void SetPermissionRequest(std::unique_ptr<PendingPermission> req);
    void SetConfirmationRequest(std::unique_ptr<PendingConfirmation> req);
    void ClearPendingFor(std::string_view request_id);

    // --- Tool execution (stream order, keyed by tool_use_id) ---
    void OnToolStarted(const v1::ToolExecutionEvent& evt);
    void OnToolOutputDelta(const std::string& tool_use_id, const std::string& chunk);
    void OnToolFinished(const std::string& tool_use_id, const v1::ToolExecutionEvent& final_evt);

    // --- Scrollback paging (keyset by seq) ---
    void ApplyScrollbackPage(const v1::GetMessagesResponse& resp);
    bool CanLoadMore() const;

private:
    void MaybeGoIdleLocked();   // mtx_ held by caller

    mutable std::mutex mtx_;

    std::string session_id_;
    std::string display_name_;   // tab/picker label; refreshed from server metadata
    SessionStateKind state_ = SessionStateKind::Idle;

    // Scrollback window
    std::vector<v1::StoredMessage> scrollback_page_;
    int64_t last_loaded_seq_ = -1;      // oldest seq loaded so far (descending)
    bool has_earlier_ = false;

    // Streaming assistant text
    std::string streaming_accumulated_;
    bool is_streaming_ = false;

    // Pending modals
    std::optional<PendingPermission> pending_permission_;
    std::optional<PendingConfirmation> pending_confirmation_;

    // Tool executions (sequential, v1 runs one at a time)
    std::vector<ToolExecutionState> tools_;

    int64_t input_tokens_ = 0;   // last turn occupancy
    int64_t output_tokens_ = 0;
};

// ============================================================================
// App-level state: ordered tabs (roots + their subagent children), focus, derived UI.
// ============================================================================

struct TabInfo {
    std::string session_id;
    std::shared_ptr<SessionState> state;      // never moves in the vector

    std::optional<std::string> parent_session_id;   // nullopt => root session
    int64_t root_ordinal = 0;         // when its ROOT tab was created (groups children)
    uint32_t spawn_seq = 0;           // global monotonic counter: stable sort key
};

class AppState {
public:
    explicit AppState(std::shared_ptr<SessionClient> client);

    struct TabSnapshot {
        std::string session_id;
        std::string display_name;      // for tab bar / picker
        SessionStateKind state = SessionStateKind::Idle;
        bool is_subagent = false;
        int64_t root_ordinal = 0;
        uint32_t spawn_seq = 0;

        // UI-derived
        char attention_glyph = '\0';     // '!' when Waiting, else nothing
        bool is_focused = false;
    };

    // --- Tab management (all thread-safe) ---

    std::string OpenTab(const SessionClient::CreateOptions& opts);

    // Attach a session already known to the server (resumes its stream). Used when
    // the picker picks an existing session, or when a child subagent starts.
    //   parent_ordinal anchors grouping; -1 => treat as new root group.
    std::string AttachTab(const std::string& session_id, int64_t parent_ordinal = -1);

    // Called by the reader thread for every ServerEvent whose payload is not bound to
    // a tab the client already owns (e.g. SubagentStarted on the PARENT's stream).
    void OnSubagentSpawned(std::string_view parent_id, const v1::SubagentStartedEvent& evt);

    void CloseTab(const std::string& session_id);
    bool HasTab(const std::string& session_id) const;

    // --- Sending on a tab's stream (UI calls these; routes ClientEvents out) ---

    void SendChatTurn(const std::string& session_id, const std::string& text);

    // --- User answers to pending prompts (UI calls these; routes ClientEvents back) ---

    void AnswerPermission(const std::string& session_id, const std::string& request_id,
                         v1::PermissionDecision decision, std::vector<std::string> granted_keys);

    void AnswerConfirmation(const std::string& session_id, const std::string& request_id,
                           const std::string& chosen_option);

    // --- Focus (keybindings cycle forward/back through tabs in render order) ---
    void CycleFocusForward();
    void CycleFocusBackward();
    void SetFocusedTab(const std::string& session_id);

    // --- Rendering ---
    std::vector<TabSnapshot> GetAllTabs() const;   // in render order

private:
    struct TabEntry {
        TabInfo info;
        std::shared_ptr<SessionClient::Stream> stream;   // persistent bidi (+ threads)
    };

    // Routes a ServerEvent to its owned tab (by event.session_id()). Handles derived
    // state (state_, tokens, pending modals). Subagent-started events are deferred and
    // applied after the lock is released so we never open a stream while holding it.
    void DispatchToTab(const std::string& session_id, const v1::ServerEvent& event);

    mutable std::mutex mtx_;
    std::vector<TabEntry> tabs_;

    int64_t next_root_ordinal_ = 0;
    uint32_t next_spawn_seq_ = 0;
    std::string focused_id_;     // active tab ("" => none)
    std::vector<std::pair<std::string, v1::SubagentStartedEvent>> pending_subagents_;   // deferred

    std::shared_ptr<SessionClient> client_;

    void FocusWithWrap(bool forward);       // shared logic for the two cycle methods
    TabEntry* FindTab(const std::string& session_id);  // unlocked, mutex held by caller
    std::shared_ptr<SessionClient::Stream> GetStream(const std::string& session_id);
    SessionState* GetState(const std::string& session_id);
};

}  // namespace minimal::client
