#pragma once

// SessionState's components (state.hpp). Plain classes owned by SessionState
// and guarded by ITS mutex: no locking here, every method runs with the
// owner's mtx_ held. The facade keeps the orchestration of events that span
// components (turn end, page apply, rewind, resync) and the derived tab
// state; each component keeps one concern's data and rules.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/client/session_types.hpp"
#include "minimal/common/units.hpp"

namespace minimal::client {

// The three modal requests a session can be parked on. Mutually exclusive:
// setting one drops the others, and every site that ends one ends all.
class PendingPrompts {
public:
    void SetPermission(PendingPermission request);
    void SetConfirmation(PendingConfirmation request);
    void SetPluginPrompt(PendingPluginPrompt request);

    // Clears whichever pending request has `request_id`; true when one did.
    bool ClearFor(std::string_view request_id);
    void ClearAll();
    bool Any() const { return permission_ || confirmation_ || plugin_prompt_; }

    const std::optional<PendingPermission>& permission() const { return permission_; }
    const std::optional<PendingConfirmation>& confirmation() const { return confirmation_; }
    const std::optional<PendingPluginPrompt>& plugin_prompt() const { return plugin_prompt_; }

private:
    std::optional<PendingPermission> permission_;
    std::optional<PendingConfirmation> confirmation_;
    std::optional<PendingPluginPrompt> plugin_prompt_;
};

// The session's tool rows (the running-tools panel's data), keyed by
// tool_use_id -- dispatch is parallel, so several rows run at once and finish
// in completion order. A BACKGROUND row is the exception every sweep makes:
// its launching call was acked, but its detached process keeps running until
// its own terminal BackgroundTaskEvent, possibly many turns later
// (IsLiveBackground).
class ToolRunTracker {
public:
    static bool IsLiveBackground(const ToolExecutionState& row) {
        return row.is_background && row.started;
    }

    void SetOutputCap(int bytes) { output_cap_bytes_ = bytes; }

    // A row with this id exists, running or finished (a redelivered start).
    bool Has(std::string_view tool_use_id) const;
    // Opens a running row.
    void Start(const v1::ToolExecutionEvent& evt);
    // Appends live output to the running row, capped at append (head + marker
    // + tail).
    void OutputDelta(const std::string& tool_use_id, const std::string& chunk);
    enum class Finished { None, Foreground, Background };
    // Records the finish on the running row. A background row stays running:
    // only its own terminal event ends it.
    Finished Finish(const std::string& tool_use_id, const v1::ToolExecutionEvent& evt);
    // Promotes/updates the row, or reopens one; true when it reopened (no
    // row existed).
    bool BackgroundTask(const v1::BackgroundTaskEvent& evt);

    // Turn end: drops every row but live background ones.
    void SweepForTurnEnd();
    // Resync: drops running FOREGROUND rows (a dropped finish event would
    // leave them running forever).
    void SweepForResync();

    // MaybeGoIdle's question: a foreground row still running.
    bool AnyForegroundRunning() const;
    bool AnyRunning() const;
    // Absent (swept) or no longer running.
    bool Done(std::string_view tool_use_id) const;
    std::optional<std::chrono::milliseconds> FastestRunningElapsed() const;
    const std::vector<ToolExecutionState>& rows() const { return rows_; }
    // The snapshot copy; see ToolRunTracker::Copy.
    std::vector<ToolExecutionState> Copy(bool include_output) const;

    // See SessionState::ToolsVersion. Readable without the owner's lock.
    std::uint64_t Version() const { return version_.load(std::memory_order_acquire); }
    void BumpVersion() { version_.fetch_add(1, std::memory_order_release); }

private:
    std::vector<ToolExecutionState> rows_;
    std::atomic<std::uint64_t> version_{0};
    int output_cap_bytes_ = 64 * common::kKiB;
};

// The transcript window: keyset pages of persisted rows (descending by seq,
// front = newest), bounded at max_messages, plus what rides with them -- the
// context compactions, the optimistic user-turn echoes the persisted rows
// retire, the "load older" latch, and the refresh bookkeeping the frame's
// refresh state machine reads (refresh_seq / refresh_succeeded / the failure
// streak / the rewind epoch).
class ScrollbackWindow {
public:
    // What an applied page proved persisted: the facade retires the live
    // segments it covers (LiveTurnBuffer::Retire).
    struct Coverage {
        bool applied = false;   // rows changed (an empty page changes nothing)
        std::set<std::string> answered_tool_ids;
        std::vector<std::string> assistant_text;
        std::vector<std::string> assistant_thinking;
    };

    void SetMaxMessages(int max_messages) { max_messages_ = max_messages; }

    // See SessionState::ApplyScrollbackPage. Resolves the refresh attempt
    // (refresh_seq) and ends any failure streak.
    Coverage ApplyPage(const v1::GetMessagesResponse& resp, bool delta);
    // See SessionState::OnSessionRewound: trims the window (and compactions)
    // to the active transcript, or clears it for a redo; drops the echoes;
    // resolves the refresh attempt and bumps the rewind epoch.
    void Rewind(std::int64_t truncated_to_seq, std::int64_t active_last_seq);
    void MergeCompaction(const v1::CompactionEvent& evt);

    // --- the optimistic echoes (Snapshot::pending_user_messages) ---
    void SubmitEcho(const std::string& text);
    void EchoQueued(std::uint64_t turn_id, const std::string& text);
    // Removed: drops the echo. Not removed (already popped): keeps it, and
    // returns true when an echo with that id exists -- the facade says the
    // cancel lost the race.
    bool EchoCanceled(std::uint64_t turn_id, bool removed);
    void ClearEchoes() { echoes_.clear(); }
    const std::vector<PendingUserMessage>& echoes() const { return echoes_; }

    // --- refresh bookkeeping ---
    // A failed attempt: resolves it and extends the streak; returns the
    // streak's length.
    int NoteFailure();
    // A superseded response (a rewind landed while it was in flight).
    void DiscardStale();
    std::uint64_t refresh_seq() const { return refresh_seq_; }
    bool refresh_succeeded() const { return refresh_succeeded_; }
    std::uint64_t rewind_epoch() const { return rewind_epoch_; }

    // --- the "load older" latch (SessionState::TryBeginOlderFetch) ---
    bool TryBeginOlderFetch();
    void EndOlderFetch() { older_fetch_pending_ = false; }

    // --- reads ---
    std::int64_t NewestSeq() const { return page_.empty() ? -1 : page_.front().seq(); }
    bool has_earlier() const { return has_earlier_; }
    std::int64_t last_loaded_seq() const { return last_loaded_seq_; }
    bool trimmed() const { return trimmed_; }
    bool gap() const { return gap_; }
    const std::shared_ptr<const std::vector<v1::StoredMessage>>& page_view() const {
        return page_shared_;
    }
    const std::shared_ptr<const std::vector<v1::CompactionEvent>>& compactions_view() const {
        return compactions_shared_;
    }

private:
    template <typename Range>
    void MergeCompactions(const Range& incoming);
    void RetireEchoes(const v1::GetMessagesResponse& resp);
    void Publish() {
        page_shared_ = std::make_shared<const std::vector<v1::StoredMessage>>(page_);
    }

    // BOUNDED: loading older pages beyond max_messages_ evicts from the
    // newest end and sets trimmed_ -- an unbounded append let a long PageUp
    // materialize a 1M-token transcript in client memory.
    std::vector<v1::StoredMessage> page_;
    // The shared, immutable view handed to snapshots; rebuilt (one copy) only
    // when rows change, so every snapshot between applies shares it.
    std::shared_ptr<const std::vector<v1::StoredMessage>> page_shared_ =
        std::make_shared<const std::vector<v1::StoredMessage>>();
    // Deduplicated on (at_seq, created_at): a live event and the page that
    // later carries the same record are one row. Ascending by at_seq.
    std::vector<v1::CompactionEvent> compactions_;
    std::shared_ptr<const std::vector<v1::CompactionEvent>> compactions_shared_ =
        std::make_shared<const std::vector<v1::CompactionEvent>>();
    std::vector<PendingUserMessage> echoes_;   // FIFO, retired by their persisted rows
    std::int64_t last_loaded_seq_ = -1;        // oldest seq loaded so far
    bool has_earlier_ = false;
    bool trimmed_ = false;
    bool gap_ = false;
    int max_messages_ = 2000;
    bool older_fetch_pending_ = false;
    std::uint64_t refresh_seq_ = 0;
    bool refresh_succeeded_ = false;
    int failures_ = 0;   // consecutive resolved-failed fetches
    std::uint64_t rewind_epoch_ = 0;
};

// The live half of the current turn: the arrival-ordered segments (text,
// thinking, tool markers) the chat view renders while the turn streams, the
// flat text/thinking accumulations retirement matches against, and the turn
// identity (turn_seq / turn_ended) that keys stream recreation and the
// turn-aware clears. Arming a new turn is reported, not acted on: whether a
// turn is IN FLIGHT is the facade's (it spans submit, the terminal events and
// the idle derivation).
class LiveTurnBuffer {
public:
    // 2 MiB of live text is far past what a human reads mid-turn; the
    // transcript is authoritative -- the turn-end refresh re-fetches the full
    // persisted text.
    static constexpr std::size_t kTextCapBytes = 2 * common::kMiB;
    // The NEWEST segments are kept; on overflow the live_view_truncated latch
    // stands (the status bar reports it) -- never a synthetic row.
    static constexpr std::size_t kMaxSegments = 100;

    // Consumes the "last turn ended" flag: true when this starts a new turn
    // (turn_seq bumped, both flat accumulations cleared).
    bool Arm();
    // A text / thinking delta. Arms the turn when it is the turn's first
    // event; returns whether it did.
    bool AppendText(std::string_view text);
    bool AppendThinking(std::string_view text);
    // A tool call's (or reopened background row's) marker in the same
    // arrival-order timeline.
    void PushToolMarker(const std::string& tool_use_id);
    void EndBlock() { is_streaming_ = false; }
    void EndTurn() {
        is_streaming_ = false;
        turn_ended_ = true;
    }

    // Turn end: each Tool marker keeps a copy of its row's final state (the
    // turn-end sweep drops the row; the marker stays until the refresh
    // retires it). Live background rows are skipped: they outlive the turn.
    void CaptureFinalTools(const std::vector<ToolExecutionState>& rows);
    // Content-matched retirement after a page apply (see
    // LiveTurnBuffer::Retire). `tool_done(id)`: whether a
    // tool's marker may retire once its result row is persisted.
    void Retire(const std::set<std::string>& answered_tool_ids,
                const std::vector<std::string>& assistant_text,
                const std::vector<std::string>& assistant_thinking,
                const std::function<bool(const std::string&)>& tool_done);
    // Retires the turns up to `turn_seq`, keeping a newer turn's material.
    void ClearIfTurn(std::uint64_t turn_seq);
    // Drops all live material (not the turn identity).
    void Clear();

    std::uint64_t turn_seq() const { return turn_seq_; }
    bool turn_ended() const { return turn_ended_; }
    bool is_streaming() const { return is_streaming_; }
    bool view_truncated() const { return view_truncated_; }
    bool has_text() const { return !text_.empty(); }
    const std::string& flat_text() const { return text_; }
    const std::string& flat_thinking() const { return thinking_; }
    // The snapshot copy: every segment, each text-bearing one with its
    // copy-on-publish view (republished only when it changed).
    std::vector<LiveTurnSegment> CopySegments() const;

private:
    void Trim();
    void PushSegment(LiveTurnSegment::Kind kind, std::uint64_t turn,
                     const std::string& tool_use_id = {});
    void AppendCapped(LiveTurnSegment& segment, std::string_view text);

    // The flat accumulations: CAPPED at kTextCapBytes (track R2) -- further
    // deltas are dropped once the truncation marker is appended.
    std::string text_;
    bool text_truncated_ = false;
    std::string thinking_;
    bool thinking_truncated_ = false;
    std::uint64_t thinking_turn_seq_ = 0;   // the turn the thinking accumulation belongs to
    std::vector<LiveTurnSegment> segments_;
    // Source for LiveTurnSegment::segment_id. Starts at 1: 0 is the
    // default-constructed "no id" value, never assigned to a real segment.
    std::uint64_t next_segment_id_ = 1;
    bool view_truncated_ = false;   // the count cap dropped older segments
    bool is_streaming_ = false;     // a text/thinking block is open
    std::uint64_t turn_seq_ = 0;    // increments when a NEW TURN's stream starts
    bool turn_ended_ = true;        // armed by the turn's end; consumed by the next turn's first event
};

}  // namespace minimal::client
