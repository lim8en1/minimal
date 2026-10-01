#pragma once

// SessionState's components (state.hpp). Plain classes owned by SessionState
// and guarded by ITS mutex: no locking here, every method runs with the
// owner's mtx_ held. The facade keeps the orchestration of events that span
// components (turn end, page apply, rewind, resync) and the derived tab
// state; each component keeps one concern's data and rules.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "minimal/client/session_types.hpp"

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
    // The snapshot copy; see SessionState::CopyToolsLocked's history.
    std::vector<ToolExecutionState> Copy(bool include_output) const;

    // See SessionState::ToolsVersion. Readable without the owner's lock.
    std::uint64_t Version() const { return version_.load(std::memory_order_acquire); }
    void BumpVersion() { version_.fetch_add(1, std::memory_order_release); }

private:
    std::vector<ToolExecutionState> rows_;
    std::atomic<std::uint64_t> version_{0};
    int output_cap_bytes_ = 64 * 1024;
};

}  // namespace minimal::client
