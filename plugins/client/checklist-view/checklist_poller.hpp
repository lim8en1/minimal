#pragma once

// The checklist-view plugin's one fetch path: a background worker that owns
// EVERY PluginCall the plugin makes. The {todo} status segment and the
// panel view both read its caches; neither surface ever calls out from a
// render path (the StatusSegment::Render "cheap, never block" discipline,
// which this plugin has always run on).

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/common/logger.hpp"
#include "checklist_model.hpp"

namespace minimal::checklist_view {

namespace plugins = minimal::client::plugins;

class ChecklistPoller {
public:
    // Borrowed, either may be null (a host without the seam): focus then
    // comes only from the segment's nudge, and the caches simply never fill.
    // `cadence` is the idle poll interval; the first iteration and every
    // focus change fetch immediately. Tests pass a short cadence to keep
    // timing assertions off the wall clock.
    explicit ChecklistPoller(plugins::ClientApi* api, plugins::FocusProvider* focus,
                             std::chrono::milliseconds cadence = std::chrono::seconds(3))
        : api_(api), focus_(focus), cadence_(cadence), worker_([this] { RunLoop(); }) {}

    ~ChecklistPoller();

    ChecklistPoller(const ChecklistPoller&) = delete;
    ChecklistPoller& operator=(const ChecklistPoller&) = delete;

    // The status segment's render-time nudge (frame rate): a focus change
    // clears the focused cache and wakes the worker for an immediate fetch,
    // so the label and the panel's focused surface never show the previous
    // tab's checklist.
    void OnFocusHint(const std::string& focused_session_id);

    // The {todo} segment's cached string. "" until a fetch lands for the
    // focused session, and immediately after a focus switch (cleared, per
    // the unfilled-segment rule in checklist_model.hpp).
    std::string FocusedText() const;

    // A consistent snapshot of the focused surface's data.
    struct FocusedSnapshot {
        std::string session_id;
        std::vector<Item> items;
        bool ever_fetched = false;  // false: no data yet (render "waiting")
        bool valid = false;         // last fetch failed: render the stale marker
        std::chrono::steady_clock::time_point fetched_at;
    };

    FocusedSnapshot Focused() const;

    // Every stored checklist by session id -- the summary surface's source.
    // Includes sessions whose tabs are closed (the store persists); the
    // panel filters to open tabs itself.
    std::map<std::string, std::vector<Item>> AllItems() const;

    bool AllValid() const;
    bool AllEverFetched() const;

    // A consistent snapshot of the summary surface's data, the AllItems()/
    // AllValid()/AllEverFetched() trio mirroring Focused()'s single-lock
    // shape: RunLoop writes all three fields together under one lock, so
    // three independently-locked reads can pair a stale `items` with a fresh
    // `valid`/`ever_fetched` (or vice versa) if a poll lands between them.
    struct AllSnapshot {
        std::map<std::string, std::vector<Item>> items;
        bool valid = false;
        bool ever_fetched = false;
    };
    AllSnapshot AllSnapshotOf() const;

private:
    void SwitchFocusLocked(const std::string& session_id);
    void RunLoop();

    std::optional<std::vector<Item>> FetchGet(const std::string& session_id) const;
    std::optional<std::map<std::string, std::vector<Item>>> FetchList() const;

    plugins::ClientApi* api_;
    plugins::FocusProvider* focus_;
    std::chrono::milliseconds cadence_;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    FocusedSnapshot focused_snapshot_;
    std::string focused_text_;
    std::map<std::string, std::vector<Item>> all_items_;
    bool all_valid_ = false;
    bool all_ever_fetched_ = false;
    bool stop_ = false;
    bool refresh_requested_ = false;

    // LAST, and it must stay last: the constructor starts RunLoop() from this
    // member's initializer, and members are constructed in declaration order.
    // Declared above mtx_ (as it was), the worker could lock a mutex -- and
    // read stop_ -- that did not exist yet: macOS reported "mutex lock
    // failed: Invalid argument" in ~1 of 5 runs of the poller tests under
    // parallel load. The destructor joins before any member is destroyed.
    std::thread worker_;
};

}  // namespace minimal::checklist_view
