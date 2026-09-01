#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "minimal/v1/common.pb.h"
#include "minimal/v1/session_service.pb.h"
#include "minimal/client/chat_model.hpp"
#include "minimal/client/session_client.hpp"

namespace minimal::client::plugins {
class AttentionState;
class ChatSurface;
}  // namespace minimal::client::plugins

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

    // Memory bounds (track R2). Defaults; AppState applies ui.json's values
    // right after construction, before any event can arrive.
    static constexpr int kDefaultScrollbackWindowMaxMessages = 2000;
    static constexpr int kDefaultToolOutputBufferBytes = 64 * 1024;
    void SetBounds(int scrollback_max_messages, int tool_output_buffer_bytes);

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

        // Bumped whenever the persisted transcript changed in a way the chat
        // view should re-fetch (new scrollback page applied, turn completed).
        // The view compares its cached value to decide a refresh; no timer.
        uint64_t transcript_version = 0;

        // True when older-page loads evicted pages from the window's NEWEST
        // end to hold the cap: the top of the window is no longer the live
        // newest message, so the view renders a boundary marker instead of a
        // silent gap. Cleared by the newest-slice refresh at turn end.
        bool scrollback_trimmed = false;

        // Increments once per streaming turn (new stream start). The frame's
        // maintain pass keys stream recreation on this instead of the old
        // buffer-size heuristic, which could splice two rapid turns.
        uint64_t turn_seq = 0;

        // The latest /-command outcome, for the transient notice line. NEVER a
        // chat row (EXTENSIBILITY §4.4) — that is a type-level property of
        // CommandNotice, not a rendering choice made here. The display/view_hint
        // fields carry the E5 CommandDisplay refinement.
        std::optional<CommandNotice> latest_notice;

        // Monotonic per-session id for latest_notice: the VIEW side-effects
        // (open the picker, show a plugin modal) are performed ONCE per notice
        // by the frame's maintain pass, keyed on this — never from Render(),
        // which runs every frame and must stay side-effect-free for view
        // routing.
        uint64_t notice_seq = 0;

        // --- Session metadata (the E5 emission, one per attach + on change) ---
        // The wire source for the status bar's {model} {agent}
        // {permission_mode} segments and the `max` half of {tokens}. Empty/0 =
        // unknown; the segment renders nothing, per the unknown-segment rule.
        std::string model;
        std::string agent_preset;
        std::string permission_mode;
        int64_t context_window_tokens = 0;
    };

    Snapshot TakeSnapshot();

    // LIGHT variant for the per-frame tab list (track R: the focused-tab pump
    // redesign): everything the tab bar and the CROSS-SESSION tools panel
    // render, WITHOUT the heavy copies -- no scrollback page, no streaming
    // text, no pending-modal data. GetAllTabs() calls this per tab per frame;
    // the deep TakeSnapshot() runs for the FOCUSED tab only.
    Snapshot TakeLightSnapshot();

    // --- Session lifecycle ---
    void OnSessionCreated(std::string name);
    void OnSessionMetadata(const v1::SessionMetadataEvent& metadata);

    // --- Streaming response ---
    void AppendStreamingText(std::string_view text);
    void FinalizeStreamingText();

    // --- Turn boundaries (ToolExecution / TextDelta set Working/Thinking; these clear it) ---
    void OnTurnCompleted(const v1::TurnCompletedEvent& evt);
    void OnError(const std::string& error_message);

    // /-command outcome. Stored as a transient notice for the notice line;
    // never enters the chat rows.
    void OnCommandResult(const v1::CommandResult& evt);

    // Drops the accumulated streaming text. The chat view calls this after a
    // post-turn scrollback refresh, when the streamed message has become a
    // persisted row -- otherwise the same content would render twice.
    void ClearStreaming();

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

    // Scrollback window (descending by seq: front = newest). BOUNDED: loading
    // older pages beyond scrollback_max_messages_ evicts from the newest end
    // and sets scrollback_trimmed_ -- an unbounded append let a long PageUp
    // materialize a 1M-token transcript in client memory.
    std::vector<v1::StoredMessage> scrollback_page_;
    int64_t last_loaded_seq_ = -1;      // oldest seq loaded so far (descending)
    bool has_earlier_ = false;
    uint64_t transcript_version_ = 0;
    bool scrollback_trimmed_ = false;
    int scrollback_max_messages_ = kDefaultScrollbackWindowMaxMessages;
    int tool_output_buffer_bytes_ = kDefaultToolOutputBufferBytes;

    // Latest /-command outcome (transient notice line; never a chat row).
    // notice_seq_ advances on every OnCommandResult so consumers can tell a
    // fresh notice from a re-snapshot of the same one.
    std::optional<CommandNotice> latest_notice_;
    uint64_t notice_seq_ = 0;

    // Session metadata (see Snapshot).
    std::string model_;
    std::string agent_preset_;
    std::string permission_mode_;
    int64_t context_window_tokens_ = 0;

    // Streaming assistant text
    std::string streaming_accumulated_;
    bool is_streaming_ = false;
    uint64_t turn_seq_ = 0;    // increments when a NEW TURN's stream starts
    bool turn_ended_ = true;   // armed by OnTurnCompleted/OnError; disarmed by the next delta

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
        char attention_glyph = '\0';     // '!' when Waiting or plugin-raised, else nothing
        bool is_focused = false;

        // The tab's tool rows (bounded buffers, R2) -- what the cross-session
        // running-tools panel renders. Filled by the LIGHT snapshot; before it
        // existed the panel forced a full deep Snapshot of EVERY tab per frame.
        std::vector<ToolExecutionState> tools;
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

    // A `/name args` prompt from the prompt editor. Split client-side (the same
    // parser the headless flag uses) and sent as a CommandInvocation; the
    // CommandResult arrives back as the tab's transient notice.
    void SendCommand(const std::string& session_id, const std::string& line);

    // Ctrl-style interrupt of the tab's in-flight turn, if any.
    void SendCancel(const std::string& session_id);

    // Synchronously re-fetches the scrollback window for a tab and applies it.
    //   newest=true: replace the window with the freshest page (post-turn).
    //   newest=false: page OLDER via the keyset cursor (scroll-up "load more").
    // Blocking on the calling thread by design -- the caller runs it as a
    // posted task inside the FTXUI loop, with a short RPC deadline; the
    // bounded pause at a fetch moment beats detached threads whose lifetime
    // would race the screen's teardown.
    void RefreshScrollback(const std::string& session_id, bool newest);

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

    // Per-tab full snapshots (tools, streaming text, notices) for the views
    // that show cross-session detail. One lock pass, no per-tab fetches.
    std::map<std::string, SessionState::Snapshot> GetAllDetails() const;

    // The focused tab's session id ("" when no tabs). The prompt editor and
    // status bar target it.
    std::string FocusedSessionId() const;

    // Closes every tab: the TUI's quit path. Terminate-all is the session
    // lifetime rule -- nothing outlives its client.
    void CloseAllTabs();

    // --- E5 extension seams (host-owned; both optional) ----------------------

    // The shared plugin-attention block (docs/EXTENSIBILITY.md 4.2): a plugin
    // raise WINS over core derivation until cleared; the glyph stays '!'.
    // Called once before any reader thread exists; not thread-of-use safe, the
    // block itself is.
    void SetPluginAttention(plugins::AttentionState* attention) {
        plugin_attention_ = attention;
    }

    // The exclusive chat surface, when a plugin claimed the slot. DispatchToTab
    // pushes EVERY ServerEvent to it (reader thread; OnEvent must not block)
    // before core's own handling -- a surface owns its presentation, core keeps
    // its data plane regardless.
    void SetChatSurface(plugins::ChatSurface* surface) { chat_surface_ = surface; }

    // The exclusive chat surface, unless a plugin threw out of it (catch +
    // disable): null means the core chat view renders the tab body.
    plugins::ChatSurface* EffectiveChatSurface() const {
        return chat_surface_broken_ ? nullptr : chat_surface_;
    }

    // Disables a surface that threw mid-render (loop thread). `reason` may be
    // null for a non-standard exception.
    void DisableChatSurface(const char* reason);

    // Memory bounds for every SessionState created AFTER this call
    // (OpenTab/AttachTab). 0 leaves that bound at its built-in default.
    // RunTui applies ui.json's values right after construction, before any
    // tab can open.
    void SetUiBounds(int scrollback_max_messages, int tool_output_buffer_bytes);

    // DEEP snapshot of ONE tab -- the focused tab, for the frame. The pre-R
    // frame called GetAllDetails() (a deep copy of EVERY tab, under each tab's
    // lock) two to three times per frame; with the light tab list (GetAllTabs
    // -> TakeLightSnapshot) and this, a frame copies the focused tab once.
    SessionState::Snapshot GetDetails(const std::string& session_id) const;

    // Monotonic UI revision, bumped by every mutating entry point. The 20 Hz
    // pump posts a redraw ONLY when this moved (plus a low-rate floor for the
    // editor's debounce gate) -- an idle TUI previously repainted the full
    // frame 20 times per second no matter what.
    uint64_t UiVersion() const {
        return ui_version_.load(std::memory_order_acquire);
    }

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

    // ui.json bounds (0 = built-in default), applied to every new SessionState.
    int scrollback_max_messages_ = 0;
    int tool_output_buffer_bytes_ = 0;

    std::atomic<uint64_t> ui_version_{0};
    void BumpUiVersion() { ui_version_.fetch_add(1, std::memory_order_acq_rel); }
    std::vector<std::pair<std::string, v1::SubagentStartedEvent>> pending_subagents_;   // deferred

    std::shared_ptr<SessionClient> client_;

    // E5 seams (see the setters above).
    plugins::AttentionState* plugin_attention_ = nullptr;
    plugins::ChatSurface* chat_surface_ = nullptr;
    // A plugin surface that THREW is disabled for the process (catch +
    // disable): the core chat view takes over, the process survives. Third
    // thrown events downgrade to a logged error, not a dead tab.
    bool chat_surface_broken_ = false;

    void FocusWithWrap(bool forward);       // shared logic for the two cycle methods
    TabEntry* FindTab(const std::string& session_id);  // unlocked, mutex held by caller
    std::shared_ptr<SessionClient::Stream> GetStream(const std::string& session_id);
    // OWNING copy: a raw pointer here escaped the lock and could be destroyed
    // by CloseTab (which erases the TabEntry and drops the shared_ptr) while
    // the reader thread was still between the lookup and the call.
    std::shared_ptr<SessionState> GetState(const std::string& session_id) const;
};

}  // namespace minimal::client
