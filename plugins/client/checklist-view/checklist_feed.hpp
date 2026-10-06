#pragma once

// checklist-view's one data path: a background worker holding a single
// PluginSubscribe "changes" stream to the checklist server plugin
// (plugins/server/checklist/checklist_hub.hpp) and caching each session's
// items. The {todo} segment and the panel only ever read that cache (the
// StatusSegment "cheap, never block" contract), so a switch to a tab already
// seen shows its list in the same frame -- no blank gap waiting on a fetch.
//
// THE STREAM is session-agnostic: every write the model makes arrives as a
// push of that session's whole list (plus a heartbeat while idle). Pushes are
// authoritative -- an empty list clears the entry. Sessions the stream has
// not pushed since this client subscribed (open tabs idle the whole time,
// the focus hint) are SEEDED with one filtered `list {"session_ids":[...]}`
// call from inside the payload callback, at most one heartbeat after they
// appear. A failed seed is retried no sooner than `poll_interval`.
//
// FALLBACK: a failed or ended stream (a server without the endpoint, a
// restart, transport loss) marks every cached entry stale -- rendered with a
// marker, never passed off as current -- then polls the open tabs' lists
// every `poll_interval` (a successful poll clears the mark) for the fallback
// period before subscribing again.
// Consecutive stream failures double that period up to `max_fallback_period`,
// and consecutive failed polls double the poll wait up to the same cap, so a
// server without the checklist plugin at all is not hammered. A server that
// only offers ".rpc" (or ignores `session_ids`) still works through the polls.
//
// TEARDOWN: the subscription can only end from its payload callback, so
// Stop() waits for the next payload -- bounded by the server's heartbeat --
// or for one in-flight PluginCall (5 s deadline). Bounded either way; never
// detached (see background_poller.hpp).

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "checklist_model.hpp"
#include "minimal/client/plugins/background_poller.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::checklist_view {

namespace plugins = minimal::client::plugins;

class ChecklistFeed : private plugins::BackgroundPoller {
public:
    struct Timing {
        std::chrono::milliseconds poll_interval = minimal::client::kPluginFeedPollInterval;
        std::chrono::milliseconds fallback_period = minimal::client::kPluginFeedFallbackPeriod;
        std::chrono::milliseconds max_fallback_period = minimal::client::kPluginFeedMaxFallbackPeriod;
    };

    // Borrowed; either may be null. No ClientApi: nothing ever fills. No
    // FocusProvider: only the focus hint's session is seeded or polled, and
    // entries for closed tabs are never evicted (harmless, just unbounded).
    ChecklistFeed(plugins::ClientApi* api, plugins::FocusProvider* focus, Timing timing);
    explicit ChecklistFeed(plugins::ClientApi* api, plugins::FocusProvider* focus = nullptr)
        : ChecklistFeed(api, focus, Timing{}) {}
    // The host's repaint request (HostUi), called after new data lands --
    // outside the feed's lock. Optional: null means the data shows at the next
    // frame something else causes.
    void SetHostUi(plugins::HostUi* ui) { ui_.store(ui, std::memory_order_release); }
    ~ChecklistFeed();

    ChecklistFeed(const ChecklistFeed&) = delete;
    ChecklistFeed& operator=(const ChecklistFeed&) = delete;

    // The {todo} segment's text for the session ("" when unknown or empty,
    // the unfilled-segment rule), stale-marked when the stream is down. Also
    // the focus hint: a changed session id wakes the fallback poller at once.
    std::string SegmentText(const std::string& focused_session_id) const;
    // The focus hint, pushed (an Observer's OnFocusChanged): the poller
    // follows the focused session even while the segment is not rendered.
    void SetFocus(const std::string& session_id) const;

    struct SessionSnapshot {
        std::vector<Item> items;
        bool known = false;  // false: nothing fetched yet (render "fetching")
        bool stale = false;  // the stream is down; last-known items
    };
    SessionSnapshot Session(const std::string& session_id) const;

private:
    struct Entry {
        std::vector<Item> items;
        bool stale = false;
    };

    void RunLoop();
    bool OnPayload(const std::string& json_payload);
    void Seed();
    void PollFallback(std::chrono::milliseconds period);
    // The sessions worth caching: every open tab's plus the focus hint's.
    // Calls the FocusProvider, so never under mtx_.
    std::set<std::string> WantedSessions() const;
    // One filtered `list` call. nullopt: the call failed. Otherwise every
    // requested id maps to its list -- empty when the server had none.
    std::optional<std::map<std::string, std::vector<Item>>> FetchLists(
        const std::set<std::string>& session_ids) const;

    plugins::ClientApi* api_;
    plugins::FocusProvider* focus_;
    // Atomic: set after the constructor has already started the worker.
    std::atomic<plugins::HostUi*> ui_{nullptr};
    void RequestRedraw() const {
        if (auto* ui = ui_.load(std::memory_order_acquire)) ui->RequestRedraw();
    }
    Timing timing_;

    // mtx_/cv_/stop_ live in BackgroundPoller (protected base members),
    // constructed before any member below -- see background_poller.hpp.
    // mutable: SegmentText() is const and records the focus hint.
    mutable std::string focused_session_id_;
    mutable bool refresh_requested_ = false;
    std::map<std::string, Entry> cache_;
    std::chrono::steady_clock::time_point next_seed_at_{};
};

}  // namespace minimal::checklist_view
