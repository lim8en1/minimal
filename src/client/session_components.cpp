#include "minimal/client/session_components.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {
// Appended ONCE when a streaming buffer (flat accumulation or a single live
// segment) hits LiveTurnBuffer::kTextCapBytes; further deltas are dropped
// after it -- the transcript is authoritative, the turn-end refresh
// re-fetches the full persisted text.
constexpr std::string_view kStreamingTruncationMarker =
    "\n··· live text truncated -- the full text is in the transcript ···\n";

// Appends to a capped flat accumulation: truncate once, append the marker,
// drop further deltas (appending past the cap would make it decorative).
void AppendFlatCapped(std::string& buffer, bool& truncated, std::string_view text) {
    if (truncated) return;
    buffer.append(text);
    if (buffer.size() > LiveTurnBuffer::kTextCapBytes) {
        buffer = TruncateUtf8(buffer, LiveTurnBuffer::kTextCapBytes);
        buffer.append(kStreamingTruncationMarker);
        truncated = true;
    }
}
}  // namespace

// --- PendingPrompts ------------------------------------------------------------

void PendingPrompts::SetPermission(PendingPermission request) {
    ClearAll();
    permission_ = std::move(request);
}

void PendingPrompts::SetConfirmation(PendingConfirmation request) {
    ClearAll();
    confirmation_ = std::move(request);
}

void PendingPrompts::SetPluginPrompt(PendingPluginPrompt request) {
    ClearAll();
    plugin_prompt_ = std::move(request);
}

bool PendingPrompts::ClearFor(std::string_view request_id) {
    bool cleared = false;
    if (permission_ && permission_->request_id == request_id) {
        permission_.reset();
        cleared = true;
    }
    if (confirmation_ && confirmation_->request_id == request_id) {
        confirmation_.reset();
        cleared = true;
    }
    if (plugin_prompt_ && plugin_prompt_->request_id == request_id) {
        plugin_prompt_.reset();
        cleared = true;
    }
    return cleared;
}

void PendingPrompts::ClearAll() {
    permission_.reset();
    confirmation_.reset();
    plugin_prompt_.reset();
}

// --- ToolRunTracker --------------------------------------------------------------
// Every handler keys on tool_use_id, never on position: dispatch is parallel
// (ARCHITECTURE.md "Tool dispatch is parallel", width 4 by default), and
// start/delta/finish events interleave and resolve in completion order.

bool ToolRunTracker::Has(std::string_view tool_use_id) const {
    return std::any_of(rows_.begin(), rows_.end(), [&](const ToolExecutionState& row) {
        return row.tool_use_id == tool_use_id;
    });
}

void ToolRunTracker::Start(const v1::ToolExecutionEvent& evt) {
    ToolExecutionState tool;
    tool.tool_use_id = evt.tool_use_id();
    tool.tool_name = evt.tool_name();
    tool.summary = evt.summary();
    tool.input = StructToJson(evt.input());
    tool.started_at_unix_ms = std::chrono::milliseconds(evt.started_at_unix_ms());
    tool.started = true;
    rows_.push_back(std::move(tool));
    BumpVersion();   // a new row the panel must show
}

void ToolRunTracker::OutputDelta(const std::string& tool_use_id, const std::string& chunk) {
    for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
        if (it->tool_use_id != tool_use_id || !it->started) continue;   // the active execution
        it->buffered_output_working.append(chunk);
        // Cap AT APPEND (track R2): render-time truncation alone let a chatty
        // tool (a build, a `tail -f`) grow client memory for the whole turn.
        // Head 1/4 + marker + tail 3/4, mirroring the server's persisted-result
        // cap. An EXACT per-delta bound, not amortized: the running-tools
        // panel renders mid-stream, so a transient overshoot is visible
        // (session_state_test.cpp, "tool output deltas stay bounded").
        const auto cap = static_cast<std::size_t>(output_cap_bytes_);
        if (it->buffered_output_working.size() > cap) {
            static const std::string kMarker =
                "\n··· live output trimmed -- the full result is in the transcript ···\n";
            const std::size_t head_bytes = cap / 4;
            // Defensive floor: ui_config.cpp rejects a bound below the
            // marker's own size, but this is the site that would underflow
            // (size_t wraparound threw an out-of-range substr() on the gRPC
            // reader thread) -- a plain keep-the-tail truncation is always
            // safe. Utf8CeilToBoundary on both tail cuts: a raw byte offset
            // can land inside a multi-byte sequence.
            if (head_bytes + kMarker.size() >= cap) {
                const std::size_t size = it->buffered_output_working.size();
                const std::size_t start = size > cap ? size - cap : 0;
                it->buffered_output_working = it->buffered_output_working.substr(
                    Utf8CeilToBoundary(it->buffered_output_working, start));
            } else {
                const std::size_t tail_budget = cap - head_bytes - kMarker.size();
                const std::size_t tail_start = Utf8CeilToBoundary(
                    it->buffered_output_working, it->buffered_output_working.size() - tail_budget);
                it->buffered_output_working =
                    TruncateUtf8(it->buffered_output_working, head_bytes) + kMarker +
                    it->buffered_output_working.substr(tail_start);
            }
        }
        it->output_dirty = true;
        BumpVersion();   // the panel's live-tail rows changed
        return;
    }
}

ToolRunTracker::Finished ToolRunTracker::Finish(const std::string& tool_use_id,
                                                const v1::ToolExecutionEvent& evt) {
    for (auto it = rows_.rbegin(); it != rows_.rend(); ++it) {
        if (it->tool_use_id != tool_use_id || !it->started) continue;   // the matching active tool
        it->is_error = evt.has_is_error() ? evt.is_error() : false;
        it->duration = std::chrono::milliseconds(evt.duration_ms());
        BumpVersion();   // status/duration the panel shows
        // A BACKGROUND row's launching call finishing means only its
        // synchronous half returned; the process keeps running
        // (ARCHITECTURE.md "Background bash rows"). Flipping `started` here
        // would drop its live deltas and read it as done.
        if (it->is_background) return Finished::Background;
        it->started = false;
        return Finished::Foreground;
    }
    return Finished::None;
}

// ARCHITECTURE.md "Background bash rows": the event rides the SAME id as the
// ToolExecutionEvent that acked the launching call, so STARTED typically finds
// the row Start opened and promotes it. A row already finished or swept
// REOPENS ("a client keyed by tool_use_id alone must tolerate a row reopening
// after its finish event").
bool ToolRunTracker::BackgroundTask(const v1::BackgroundTaskEvent& evt) {
    const bool is_terminal = evt.state() != v1::BackgroundTaskEvent::STARTED;
    const bool is_error = evt.state() == v1::BackgroundTaskEvent::FAILED ||
                          evt.state() == v1::BackgroundTaskEvent::TIMED_OUT ||
                          evt.state() == v1::BackgroundTaskEvent::KILLED;
    BumpVersion();   // a promoted, updated or reopened row
    for (auto& row : rows_) {
        if (row.tool_use_id != evt.tool_use_id()) continue;
        row.is_background = true;
        row.started = !is_terminal;
        if (is_terminal) row.is_error = is_error;
        if (!evt.command().empty()) row.summary = evt.command();
        return false;
    }
    ToolExecutionState row;
    row.tool_use_id = evt.tool_use_id();
    row.tool_name = evt.tool_name();
    row.summary = evt.command();
    row.is_background = true;
    row.started = !is_terminal;
    row.is_error = is_terminal && is_error;
    rows_.push_back(std::move(row));
    return true;
}

void ToolRunTracker::SweepForTurnEnd() {
    const std::size_t before = rows_.size();
    std::erase_if(rows_, [](const ToolExecutionState& row) { return !IsLiveBackground(row); });
    if (rows_.size() != before) BumpVersion();
}

void ToolRunTracker::SweepForResync() {
    const std::size_t before = rows_.size();
    std::erase_if(rows_, [](const ToolExecutionState& row) {
        return row.started && !row.is_background;
    });
    if (rows_.size() != before) BumpVersion();
}

bool ToolRunTracker::AnyForegroundRunning() const {
    return std::any_of(rows_.begin(), rows_.end(), [](const ToolExecutionState& row) {
        return row.started && !row.is_background;
    });
}

bool ToolRunTracker::AnyRunning() const {
    return std::any_of(rows_.begin(), rows_.end(),
                       [](const ToolExecutionState& row) { return row.started; });
}

bool ToolRunTracker::Done(std::string_view tool_use_id) const {
    for (const auto& row : rows_) {
        if (row.tool_use_id == tool_use_id) return !row.started;
    }
    return true;   // swept: by definition done
}

std::optional<std::chrono::milliseconds> ToolRunTracker::FastestRunningElapsed() const {
    std::optional<std::chrono::milliseconds> newest_start;
    for (const auto& row : rows_) {
        if (!row.started || row.started_at_unix_ms.count() < 0) continue;
        if (!newest_start.has_value() || row.started_at_unix_ms > *newest_start) {
            newest_start = row.started_at_unix_ms;
        }
    }
    if (!newest_start.has_value()) return std::nullopt;
    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return std::max(std::chrono::milliseconds(0), now_ms - *newest_start);
}

std::vector<ToolExecutionState> ToolRunTracker::Copy(bool include_output) const {
    // Field-by-field, ALWAYS: the working buffer (up to the output cap per
    // tool) never leaves the tracker. With include_output, the copy carries
    // `buffered_output` -- a shared, immutable view republished (one copy)
    // only when the working buffer changed since the last publish, so
    // repeated fetches between deltas cost a refcount bump per tool.
    std::vector<ToolExecutionState> out;
    out.reserve(rows_.size());
    for (const auto& row : rows_) {
        ToolExecutionState t;
        t.tool_use_id = row.tool_use_id;
        t.tool_name = row.tool_name;
        t.summary = row.summary;
        t.input = row.input;
        t.started_at_unix_ms = row.started_at_unix_ms;
        t.duration = row.duration;
        t.started = row.started;
        t.is_error = row.is_error;
        t.is_background = row.is_background;
        if (include_output) {
            if (row.buffered_output == nullptr || row.output_dirty) {
                row.buffered_output = std::make_shared<const std::string>(row.buffered_output_working);
                row.output_dirty = false;
            }
            t.buffered_output = row.buffered_output;
        }
        out.push_back(std::move(t));
    }
    return out;
}

// --- ScrollbackWindow --------------------------------------------------------------

template <typename Range>
void ScrollbackWindow::MergeCompactions(const Range& incoming) {
    bool changed = false;
    for (const auto& evt : incoming) {
        const bool held = std::any_of(compactions_.begin(), compactions_.end(),
                                      [&evt](const v1::CompactionEvent& have) {
                                          return have.at_seq() == evt.at_seq() &&
                                                 have.created_at_unix_ms() ==
                                                     evt.created_at_unix_ms();
                                      });
        if (held) continue;
        compactions_.push_back(evt);
        changed = true;
    }
    if (!changed) return;
    std::stable_sort(compactions_.begin(), compactions_.end(),
                     [](const v1::CompactionEvent& a, const v1::CompactionEvent& b) {
                         return a.at_seq() != b.at_seq()
                                    ? a.at_seq() < b.at_seq()
                                    : a.created_at_unix_ms() < b.created_at_unix_ms();
                     });
    compactions_shared_ = std::make_shared<const std::vector<v1::CompactionEvent>>(compactions_);
}

void ScrollbackWindow::MergeCompaction(const v1::CompactionEvent& evt) {
    MergeCompactions(std::array<v1::CompactionEvent, 1>{evt});
}

ScrollbackWindow::Coverage ScrollbackWindow::ApplyPage(const v1::GetMessagesResponse& resp,
                                                       bool delta) {
    Coverage coverage;
    refresh_succeeded_ = true;   // resolved OK, even when the page is empty
    failures_ = 0;               // a success ends any failure streak (and its notice)
    MergeCompactions(resp.compactions());
    if (resp.messages().empty()) {
        // A delta fetch that returns nothing proves nothing about OLDER
        // history -- has_more on an after_seq response means "more newer
        // rows", which is definitionally false for an empty one. The
        // load-older affordance must not move.
        if (!delta) has_earlier_ = resp.has_more();
        ++refresh_seq_;   // a resolved attempt (successful RPC, empty page)
        return coverage;
    }

    // Set when the newest-slice assign below trims the fetched page to the
    // configured cap -- distinct from trimmed_, which specifically means
    // NEWER rows were evicted; this is the opposite direction (we chose to
    // keep fewer than the full fetch), so it only needs to correct
    // has_earlier_.
    bool assign_capped = false;
    if (delta) {
        // ASCENDING rows newer than the cursor the fetch was submitted with:
        // merge onto the window's newest end, never replacing it. Rows the
        // window already holds (a racing duplicate fetch, or the window's
        // front having moved past part of the page between submit and apply)
        // are skipped; a discontinuity against the held newest seq records
        // gap_ exactly as the older-page stitch below does -- rows are only
        // ever APPENDED server-side, so a hole here means rows were discarded
        // (a rewind race that the epoch guard missed, or a redo) and a full
        // refresh must reconcile the window.
        const std::int64_t held_newest = NewestSeq();
        std::vector<v1::StoredMessage> fresh;   // ascending -> reversed into desc
        for (const auto& stored : resp.messages()) {
            if (stored.seq() <= held_newest) continue;   // already held
            fresh.push_back(stored);
        }
        if (!fresh.empty()) {
            std::int64_t expected_prev = held_newest;   // each accepted row must be exactly one newer
            for (const auto& stored : fresh) {
                if (stored.seq() != expected_prev + 1) gap_ = true;
                expected_prev = stored.seq();
            }
            // Ascending -> the descending window's front, in ONE insert: a
            // row-at-a-time insert(begin()) shifted the whole window per row.
            page_.insert(page_.begin(), fresh.rbegin(), fresh.rend());
            // Caught up to the server's newest: anything an older-page load
            // evicted from the window's NEWEST end is visible again. (A gap
            // INSIDE the window is a different flag and stays.)
            if (!resp.has_more()) trimmed_ = false;
            // Sliding window, oldest end only. Rows the cap evicts there are
            // fetchable earlier pages -- the conservative rule Rewind applies
            // (true costs at most one empty load-older round trip). Without
            // it, a window that grew up from the transcript's first rows kept
            // has_earlier_ false forever and "load older" was wedged.
            if (static_cast<int>(page_.size()) > max_messages_) {
                page_.resize(static_cast<std::size_t>(max_messages_));
                has_earlier_ = true;
            }
        }
    } else if (page_.empty() || resp.messages(0).seq() > page_.front().seq()) {
        // Newer than what we hold -- keep only the contiguous newest slice.
        page_.assign(resp.messages().begin(), resp.messages().end());
        trimmed_ = false;
        gap_ = false;   // same reset as the newest-slice refresh's other boundary flag
        // RequestScrollback always asks for up to 100 rows, independent of
        // max_messages_ -- without this, a newest apply could exceed the cap
        // outright, and the next older-page load's eviction would evict rows
        // just shown instead of only the ones it fetched.
        if (static_cast<int>(page_.size()) > max_messages_) {
            page_.resize(static_cast<std::size_t>(max_messages_));
            assign_capped = true;
        }
    } else {
        // Older than (or overlapping) what we hold: append, SKIPPING rows we
        // already hold. The assign branch above must only ever fire when the
        // page's newest seq is genuinely newer than the window's -- a
        // duplicate in-flight "load older" fetch returns the same before_seq
        // page, and its apply taking the assign branch REPLACED the window
        // with the older page: the newest rows vanished from the view.
        const std::int64_t oldest_held = page_.back().seq();
        // Contiguity stitch: walking the page's DESC-order rows, each must be
        // exactly one below the previous seq we hold/accepted. A row BELOW
        // that expectation means seqs are missing between window and page --
        // a server-side hole the eviction marker cannot describe; gap_
        // records it, and the newest-slice refresh clears both.
        std::int64_t expected_prev = oldest_held - 1;
        for (const auto& stored : resp.messages()) {
            if (stored.seq() >= oldest_held) continue;   // already held (or a raced duplicate)
            if (stored.seq() > expected_prev) continue;  // duplicate within this page
            if (stored.seq() < expected_prev) gap_ = true;
            expected_prev = stored.seq() - 1;
            page_.push_back(stored);
        }
        // Sliding window (track R2): an older-page load evicts from the
        // NEWEST end to hold the cap -- flagged (trimmed_) and rendered as a
        // boundary marker, never a silent gap. A delta fetch that catches
        // back up to the server's newest clears it (the branch above).
        if (static_cast<int>(page_.size()) > max_messages_) {
            const auto excess = page_.size() - static_cast<std::size_t>(max_messages_);
            page_.erase(page_.begin(), page_.begin() + static_cast<std::ptrdiff_t>(excess));
            trimmed_ = true;
        }
    }

    // What the applied rows prove persisted (content-matched retirement --
    // see LiveTurnBuffer::Retire).
    for (const auto& stored : resp.messages()) {
        const bool assistant = stored.message().role() == v1::ROLE_ASSISTANT;
        for (const auto& block : stored.message().content()) {
            if (block.has_tool_result()) {
                coverage.answered_tool_ids.insert(block.tool_result().tool_use_id());
            } else if (assistant && block.has_text()) {
                coverage.assistant_text.push_back(block.text().text());
            } else if (assistant && block.has_thinking()) {
                coverage.assistant_thinking.push_back(block.thinking().text());
            }
        }
    }

    last_loaded_seq_ = page_.empty() ? -1 : page_.back().seq();
    // The server's own has_more() may say false while assign_capped means we
    // kept less than the full fetch -- "load older" must not read as
    // exhausted then. A delta apply leaves the flag alone: has_more on an
    // after_seq response speaks about NEWER rows.
    if (!delta) has_earlier_ = resp.has_more() || assign_capped;
    // The one copy the shared page ever pays: rows changed, so the old
    // snapshot's view must stay stable for whatever frame is still reading it.
    Publish();
    RetireEchoes(resp);
    ++refresh_seq_;
    coverage.applied = true;
    return coverage;
}

// Retire optimistic echoes whose persisted row just landed. Entries pair with
// their rows in FIFO order (the server queue is FIFO; each entry produces
// exactly one text-bearing user row when its turn pops), so the applied
// page's text-bearing USER rows newer than the OLDEST entry's baseline retire
// entries front-to-back, one row per entry. Rows the baseline filter excludes
// are the running turn's output (assistant rows) and injected
// tool-result-only user rows -- neither is an echo's row.
void ScrollbackWindow::RetireEchoes(const v1::GetMessagesResponse& resp) {
    if (echoes_.empty()) return;
    const std::int64_t oldest_baseline = echoes_.front().baseline_seq;
    std::size_t user_text_rows = 0;
    for (const auto& stored : resp.messages()) {
        if (stored.seq() <= oldest_baseline) continue;
        if (stored.message().role() != v1::ROLE_USER) continue;
        for (const auto& block : stored.message().content()) {
            if (block.has_text() && !block.text().text().empty()) {
                ++user_text_rows;
                break;
            }
        }
    }
    const auto retire_count = std::min(user_text_rows, echoes_.size());
    echoes_.erase(echoes_.begin(), echoes_.begin() + static_cast<std::ptrdiff_t>(retire_count));
}

// SessionRewoundEvent: the transcript tail was cut (rewind) or restored
// (redo). The durable record moved under the window.
void ScrollbackWindow::Rewind(std::int64_t truncated_to_seq, std::int64_t active_last_seq) {
    // Compactions that ran on the rewound-away rows go with them (a redo's
    // restored rows bring theirs back on the refetch).
    const auto keep_up_to = truncated_to_seq < 0 ? std::int64_t{-2} : active_last_seq;
    const std::size_t before = compactions_.size();
    std::erase_if(compactions_,
                  [keep_up_to](const v1::CompactionEvent& evt) { return evt.at_seq() > keep_up_to; });
    if (compactions_.size() != before) {
        compactions_shared_ = std::make_shared<const std::vector<v1::CompactionEvent>>(compactions_);
    }
    if (truncated_to_seq < 0) {
        // REDO: the restored rows are unknown client-side -- clear the whole
        // window so the refresh refetches them from the top.
        page_.clear();
        has_earlier_ = true;
    } else {
        // Descending window: the cut rows sit at the FRONT.
        const auto first_kept =
            std::find_if(page_.begin(), page_.end(), [active_last_seq](const v1::StoredMessage& m) {
                return m.seq() <= active_last_seq;
            });
        page_.erase(page_.begin(), first_kept);
        // Conservative: rows survived -> older pages may exist; setting true
        // when they don't just costs one empty "load older" round trip.
        has_earlier_ = !page_.empty() ? true : has_earlier_;
    }
    last_loaded_seq_ = page_.empty() ? -1 : page_.back().seq();
    // The window was rebuilt wholesale: any previously-detected stitching gap
    // describes rows that no longer exist in it.
    gap_ = false;
    // The rewound turn's echo has nothing left to retire it.
    echoes_.clear();
    Publish();
    // Resolved like an apply: the refresh_seq bump unsticks the frame's
    // refresh latch. Neither it nor the facade's fetch bump stops an
    // ALREADY-IN-FLIGHT fetch from applying its (now stale) response -- that
    // is the epoch's job (RequestScrollback captures it at submit time;
    // DiscardStale).
    refresh_succeeded_ = true;
    ++refresh_seq_;
    ++rewind_epoch_;
}

void ScrollbackWindow::SubmitEcho(const std::string& text) {
    PendingUserMessage entry;
    entry.text = text;
    entry.baseline_seq = NewestSeq();
    echoes_.push_back(std::move(entry));
}

void ScrollbackWindow::EchoQueued(std::uint64_t turn_id, const std::string& text) {
    // The server acks submissions in queue order; match the id to the OLDEST
    // still-unassigned echo with the same text. An event with no matching
    // echo is another client's submission -- ignored.
    for (auto& entry : echoes_) {
        if (entry.turn_id == 0 && entry.text == text) {
            entry.turn_id = turn_id;
            return;
        }
    }
}

bool ScrollbackWindow::EchoCanceled(std::uint64_t turn_id, bool removed) {
    if (!removed) {
        return std::any_of(echoes_.begin(), echoes_.end(),
                           [turn_id](const PendingUserMessage& e) { return e.turn_id == turn_id; });
    }
    for (auto it = echoes_.begin(); it != echoes_.end(); ++it) {
        if (it->turn_id == turn_id) {
            echoes_.erase(it);
            break;
        }
    }
    return false;
}

int ScrollbackWindow::NoteFailure() {
    refresh_succeeded_ = false;   // resolved with a failure
    ++refresh_seq_;               // resolved (no content change) -- a mismatch retries next frame
    return ++failures_;
}

void ScrollbackWindow::DiscardStale() {
    refresh_succeeded_ = true;
    ++refresh_seq_;
}

bool ScrollbackWindow::TryBeginOlderFetch() {
    if (older_fetch_pending_) return false;
    older_fetch_pending_ = true;
    return true;
}

// --- LiveTurnBuffer ----------------------------------------------------------------

// A NEW TURN begins only after the previous one ENDED (TurnCompleted/Error):
// message stops fire between tool cycles WITHIN a turn, so arming on
// is_streaming_ alone would re-key the stream mid-turn. The first event of the
// new turn -- text, thinking or a tool call, whichever arrives first -- bumps
// turn_seq_ and clears BOTH flat accumulations. Both: the turn-end refresh
// (async) owns clearing the previous turn's tails (ClearIfTurn), and
// whichever path arms first consumes turn_ended_ -- three copies of this once
// each cleared only their own buffer, so a thinking-first turn kept the old
// turn's text tail and fused the next text onto it.
bool LiveTurnBuffer::Arm() {
    if (!turn_ended_) return false;
    ++turn_seq_;
    turn_ended_ = false;
    text_.clear();
    text_truncated_ = false;
    thinking_.clear();
    thinking_truncated_ = false;
    return true;
}

void LiveTurnBuffer::PushSegment(LiveTurnSegment::Kind kind, std::uint64_t turn,
                                 const std::string& tool_use_id) {
    segments_.push_back(LiveTurnSegment{.kind = kind,
                                        .turn_seq = turn,
                                        .segment_id = next_segment_id_++,
                                        .tool_use_id = tool_use_id});
}

// Per-segment cap: the flat budget and shape. The APPEND is guarded, not just
// the resize: once text_truncated is set, further deltas for the segment are
// dropped entirely, or the string keeps growing past the cap (deep-copied
// every frame) without ever growing the visible text.
void LiveTurnBuffer::AppendCapped(LiveTurnSegment& segment, std::string_view text) {
    if (segment.text_truncated) return;
    segment.text.append(text);
    if (segment.text.size() > kTextCapBytes) {
        segment.text = TruncateUtf8(segment.text, kTextCapBytes);
        segment.text.append(kStreamingTruncationMarker);
        segment.text_truncated = true;
    }
    // Invalidate the copy-on-publish view (see LiveTurnSegment): the next deep
    // snapshot republishes this segment -- once, not per consumer.
    segment.text_dirty = true;
}

bool LiveTurnBuffer::AppendText(std::string_view text) {
    // Track C4: a NEW segment starts on a fresh turn (no segments), a block
    // boundary (is_streaming_ false -- the previous block's stop finalized
    // it), or when the most recent segment is a TOOL marker (text resumed
    // after a tool call) -- so the view shows text and tool activity
    // interleaved in arrival order.
    const bool new_segment = segments_.empty() || !is_streaming_ ||
                             segments_.back().kind != LiveTurnSegment::Kind::Text;
    // turn_ended_ still true means THIS append is the new turn's first -- it
    // bumps turn_seq_ below, so the segment must already carry that value.
    const std::uint64_t segment_turn = turn_ended_ ? turn_seq_ + 1 : turn_seq_;
    if (new_segment) PushSegment(LiveTurnSegment::Kind::Text, segment_turn);
    AppendCapped(segments_.back(), text);
    Trim();
    // A new BLOCK within the same turn (text -> tool -> text, thinking ->
    // text): without a separator the two blocks' raw markdown fuses into one
    // document (an unclosed fence from block one would eat block two); a
    // blank line is the break ComposeDocument puts between rendered blocks.
    if (!turn_ended_ && !is_streaming_ && !text_.empty() && !text_truncated_) {
        text_.append("\n\n");
    }
    const bool starting_new_turn = Arm();
    AppendFlatCapped(text_, text_truncated_, text);
    is_streaming_ = true;
    return starting_new_turn;
}

// Thinking gets the text path's budget and turn arming, a SEPARATE flat
// accumulation, and its own segment kind: reasoning is one of the turn's
// blocks in arrival order (typically the first).
bool LiveTurnBuffer::AppendThinking(std::string_view text) {
    // ARMING RUNS BEFORE THE APPENDS: the clear inside Arm would otherwise
    // wipe this very delta when it is the turn's first. It also stamps the
    // turn so ClearIfTurn retires (or keeps) thinking and text consistently.
    const bool starting_new_turn = Arm();
    thinking_turn_seq_ = turn_seq_;
    // Only the BYTES are dropped past the cap: the segment tracking below
    // still runs, so a turn past 2 MiB of reasoning still opens segments.
    AppendFlatCapped(thinking_, thinking_truncated_, text);
    // reasoning -> text -> reasoning stays three segments in arrival order.
    const bool new_segment = segments_.empty() || !is_streaming_ ||
                             segments_.back().kind != LiveTurnSegment::Kind::Thinking;
    if (new_segment) PushSegment(LiveTurnSegment::Kind::Thinking, turn_seq_);
    AppendCapped(segments_.back(), text);
    Trim();
    is_streaming_ = true;
    return starting_new_turn;
}

void LiveTurnBuffer::PushToolMarker(const std::string& tool_use_id) {
    PushSegment(LiveTurnSegment::Kind::Tool, turn_seq_, tool_use_id);
    // Same count cap the delta paths enforce: a tool-only run would otherwise
    // grow the segments by one marker per call with no upper bound.
    Trim();
}

// Oldest-first, but Text/Thinking go before Tool markers: a marker is a tiny
// struct, and its disappearance mid-turn is exactly the "tool calls invisible
// until finished" failure -- the persisted rows that would replace it only
// arrive with the refresh. A Tool marker is trimmed only when the turn is
// entirely tool calls. The cut itself is never rendered as a segment: the
// latch it sets is what the status bar's {activity} suffix reports.
void LiveTurnBuffer::Trim() {
    if (segments_.size() <= kMaxSegments) return;
    view_truncated_ = true;
    while (segments_.size() > kMaxSegments) {
        auto victim = std::find_if(segments_.begin(), segments_.end(), [](const LiveTurnSegment& s) {
            return s.kind != LiveTurnSegment::Kind::Tool;
        });
        if (victim == segments_.end()) victim = segments_.begin();   // all tools: drop the oldest
        segments_.erase(victim);
    }
}

void LiveTurnBuffer::Clear() {
    text_.clear();
    text_truncated_ = false;
    thinking_.clear();
    thinking_truncated_ = false;
    is_streaming_ = false;
    segments_.clear();
    view_truncated_ = false;
}

void LiveTurnBuffer::CaptureFinalTools(const std::vector<ToolExecutionState>& rows) {
    // The chat view used to render a marker whose row was swept as NOTHING;
    // whenever the post-turn refresh was late or never came, the turn's tool
    // calls simply vanished. Output buffers are not copied: the live row
    // never renders them.
    for (auto& segment : segments_) {
        if (segment.kind != LiveTurnSegment::Kind::Tool || segment.final_tool) continue;
        for (const auto& row : rows) {
            if (row.tool_use_id != segment.tool_use_id || ToolRunTracker::IsLiveBackground(row)) {
                continue;
            }
            auto copy = std::make_shared<ToolExecutionState>();
            copy->tool_use_id = row.tool_use_id;
            copy->tool_name = row.tool_name;
            copy->summary = row.summary;
            copy->input = row.input;
            copy->started_at_unix_ms = row.started_at_unix_ms;
            copy->duration = row.duration;
            copy->started = row.started;
            copy->is_error = row.is_error;
            copy->is_background = row.is_background;
            segment.final_tool = std::move(copy);
            break;
        }
    }
}

void LiveTurnBuffer::ClearIfTurn(std::uint64_t turn_seq) {
    // Retire the live material of the turns the persisted page covers (seq <=
    // the captured turn) -- keeping anything a NEWER turn has streamed since
    // the fetch was submitted. A blanket clear was the stale-clear bug (the
    // next turn lost its opening deltas); a blanket keep double-rendered the
    // retired turn against the rows that now cover it.
    const auto keep_end = std::stable_partition(
        segments_.begin(), segments_.end(),
        [turn_seq](const LiveTurnSegment& segment) { return segment.turn_seq > turn_seq; });
    if (keep_end == segments_.begin()) {
        Clear();   // nothing survives: the classic clear
        return;
    }
    // The thinking accumulation is turn-stamped: retire it exactly when the
    // persisted page covers its turn.
    if (thinking_turn_seq_ <= turn_seq) {
        thinking_.clear();
        thinking_truncated_ = false;
    }
    // Rebuild the flat text from the surviving text segments (the blank-line
    // break mirrors the within-turn block separator).
    std::string rebuilt;
    for (auto it = segments_.begin(); it != keep_end; ++it) {
        if (it->kind != LiveTurnSegment::Kind::Text || it->text.empty()) continue;
        if (!rebuilt.empty()) rebuilt.append("\n\n");
        rebuilt.append(it->text);
    }
    text_ = std::move(rebuilt);
    // The rebuild drops whatever pushed the old buffer past the cap (retired
    // segments go with their turn), so the budget is fresh.
    text_truncated_ = text_.size() > kTextCapBytes;
    segments_.erase(keep_end, segments_.end());
    // The retired segments are exactly what a count-cap overflow would have
    // dropped; once the page covering them landed, the latch no longer holds.
    view_truncated_ = false;
}

void LiveTurnBuffer::Retire(const std::set<std::string>& answered_tool_ids,
                            const std::vector<std::string>& assistant_text,
                            const std::vector<std::string>& assistant_thinking,
                            const std::function<bool(const std::string&)>& tool_done) {
    // Exact equality for Text/Thinking: the live segment IS the concatenation
    // of the block's deltas, and the persisted block is the same block
    // server-side. A cap-truncated segment never equals its block (the marker
    // tail is client-side presentation loss), so it matches by prefix, with
    // the marker stripped before comparing.
    const auto text_matches = [](const LiveTurnSegment& segment,
                                 const std::vector<std::string>& blocks) {
        if (blocks.empty()) return false;
        if (segment.text_truncated) {
            std::string_view prefix = segment.text;
            const auto cut = prefix.rfind(kStreamingTruncationMarker);
            if (cut != std::string_view::npos) prefix.remove_suffix(prefix.size() - cut);
            if (prefix.empty()) return false;
            return std::any_of(blocks.begin(), blocks.end(), [prefix](const std::string& block) {
                return block.size() >= prefix.size() &&
                       std::string_view(block).substr(0, prefix.size()) == prefix;
            });
        }
        return std::find(blocks.begin(), blocks.end(), segment.text) != blocks.end();
    };
    const auto covered = [&](const LiveTurnSegment& segment) {
        if (segment.kind == LiveTurnSegment::Kind::Tool) {
            // A still-running tool never retires even though its result row
            // reached the page: only its ACK result is persisted (a
            // background row's process keeps producing output; a foreground
            // row still on the pool has not finished).
            return answered_tool_ids.contains(segment.tool_use_id) && tool_done(segment.tool_use_id);
        }
        return text_matches(segment, segment.kind == LiveTurnSegment::Kind::Text
                                         ? assistant_text
                                         : assistant_thinking);
    };
    const auto keep_end = std::stable_partition(
        segments_.begin(), segments_.end(), [&](const LiveTurnSegment& s) { return !covered(s); });
    if (keep_end == segments_.begin()) {
        Clear();   // the turn's whole live tail is covered by the page
        return;
    }
    // Rebuild the flat accumulations from the survivors, in the shapes the
    // append paths maintain: text joined by the blank block separator,
    // thinking concatenated without one.
    std::string rebuilt_text;
    std::string rebuilt_thinking;
    for (auto it = segments_.begin(); it != keep_end; ++it) {
        if (it->kind == LiveTurnSegment::Kind::Text && !it->text.empty()) {
            if (!rebuilt_text.empty()) rebuilt_text.append("\n\n");
            rebuilt_text.append(it->text);
        } else if (it->kind == LiveTurnSegment::Kind::Thinking) {
            rebuilt_thinking.append(it->text);
        }
    }
    text_ = std::move(rebuilt_text);
    text_truncated_ = text_.size() > kTextCapBytes;
    thinking_ = std::move(rebuilt_thinking);
    thinking_truncated_ = thinking_.size() > kTextCapBytes;
    // is_streaming_ describes the OPEN block, which is by construction the
    // LAST segment (a tool marker is only pushed after its block closed). If
    // the surviving tail ends on a Tool marker, the open-block notion goes
    // with it, so the next delta opens a fresh segment instead of fusing onto
    // a retired one. Checked on the SURVIVORS (after the erase):
    // stable_partition moved the covered segments to the end.
    segments_.erase(keep_end, segments_.end());
    if (!segments_.empty() && segments_.back().kind == LiveTurnSegment::Kind::Tool) {
        is_streaming_ = false;
    }
    view_truncated_ = false;   // see ClearIfTurn
}

std::vector<LiveTurnSegment> LiveTurnBuffer::CopySegments() const {
    // Field-copy: the working `text` never leaves the buffer. Each
    // text-bearing segment's copy-on-publish view is rebuilt -- one copy of
    // that ONE segment -- only when it changed since the last deep snapshot,
    // so settled segments share their view and only the newest pays per delta.
    std::vector<LiveTurnSegment> out;
    out.reserve(segments_.size());
    for (const auto& segment : segments_) {
        LiveTurnSegment s;
        s.kind = segment.kind;
        s.turn_seq = segment.turn_seq;
        s.segment_id = segment.segment_id;
        s.text_truncated = segment.text_truncated;
        s.tool_use_id = segment.tool_use_id;
        s.final_tool = segment.final_tool;
        if ((segment.kind == LiveTurnSegment::Kind::Text ||
             segment.kind == LiveTurnSegment::Kind::Thinking) &&
            !segment.text.empty()) {
            if (segment.text_view == nullptr || segment.text_dirty) {
                segment.text_view = std::make_shared<const std::string>(segment.text);
                segment.text_dirty = false;
            }
            s.text_view = segment.text_view;
        }
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace minimal::client
