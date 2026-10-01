#include "minimal/client/session_components.hpp"

#include <algorithm>
#include <utility>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

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
    row.tool_name = "bash";   // background tasks are bash's own extension
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

}  // namespace minimal::client
