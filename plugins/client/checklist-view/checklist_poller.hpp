#pragma once

// The checklist-view plugin's one fetch path: a background worker that owns
// EVERY PluginCall the plugin makes. The {todo} status segment and the
// panel view both read its caches; neither surface ever calls out from a
// render path (the StatusSegment::Render "cheap, never block" discipline,
// which this plugin has always run on).

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/client/plugins/background_poller.hpp"
#include "minimal/common/logger.hpp"
#include "checklist_model.hpp"

namespace minimal::checklist_view {

namespace plugins = minimal::client::plugins;

class ChecklistPoller : private plugins::BackgroundPoller {
public:
    // Borrowed, either may be null (a host without the seam): focus then
    // comes only from the segment's nudge, and the caches simply never fill.
    // `cadence` is the idle poll interval; the first iteration and every
    // focus change fetch immediately. Tests pass a short cadence to keep
    // timing assertions off the wall clock.
    explicit ChecklistPoller(plugins::ClientApi* api, plugins::FocusProvider* focus,
                             std::chrono::milliseconds cadence = std::chrono::seconds(3))
        : api_(api), focus_(focus), cadence_(cadence) {
        // Started from the constructor BODY, not an initializer -- every
        // member RunLoop touches (base's mtx_/cv_/stop_, and every member
        // below) is guaranteed to exist by now. See background_poller.hpp.
        Start([this] { RunLoop(); });
    }

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

    // mtx_/cv_/stop_ live in BackgroundPoller (protected base members),
    // constructed before any member below -- see background_poller.hpp.
    FocusedSnapshot focused_snapshot_;
    std::string focused_text_;
    std::map<std::string, std::vector<Item>> all_items_;
    bool all_valid_ = false;
    bool all_ever_fetched_ = false;
    bool refresh_requested_ = false;
};

}  // namespace minimal::checklist_view
