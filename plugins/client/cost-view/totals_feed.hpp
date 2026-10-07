#pragma once

// cost-view's one data path: a background worker holding a single
// PluginSubscribe "totals" stream to cost-tracker (plugins/server/
// cost-tracker/totals_hub.hpp) and caching the formatted {cost} /
// {token_detail} strings PER SESSION. Render() only ever reads that cache
// (the StatusSegment "cheap, never block" contract), so a tab switch to a
// session already seen shows its figures in the same frame -- no blank gap
// waiting on a fetch.
//
// THE STREAM is session-agnostic: every dirty session's totals arrive as
// pushes (plus a heartbeat while idle). Pushes are authoritative -- a
// "turns: 0" push (a session rewound to nothing) clears the entry. A
// session the stream has not pushed since this client subscribed (a tab
// idle the whole time) is SEEDED with one PluginCall from inside the
// payload callback, the next time any payload arrives while it is focused
// -- at most one heartbeat interval after the switch.
//
// FALLBACK: a failed or ended stream (a server without the endpoint, a
// restart, transport loss) marks every cached entry stale -- rendered with
// kStaleSuffix, never passed off as current -- then polls the focused
// session with PluginCall every `poll_interval` for `fallback_period`
// before trying to subscribe again. A server that only offers the older
// ".rpc" path keeps working through the polls.
//
// TEARDOWN: the subscription can only end from its payload callback, so
// Stop() waits for the next payload -- bounded by the server's heartbeat --
// or for one in-flight PluginCall (5 s deadline). Bounded either way; never
// detached (see background_poller.hpp).

#include <atomic>
#include <chrono>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/client/sdk/background_poller.hpp"
#include "minimal/client/sdk/host.hpp"
#include "minimal/client/sdk/render.hpp"
#include "minimal/client/sdk/policy.hpp"

namespace minimal::client::cost_view {

namespace plugins = minimal::client::plugins;

enum class TotalsField { kCost, kTokenDetail };

class TotalsFeed : private plugins::BackgroundPoller {
public:
    struct Timing {
        std::chrono::milliseconds poll_interval = client::kPluginFeedPollInterval;
        std::chrono::milliseconds fallback_period = client::kPluginFeedFallbackPeriod;
    };

    // Borrowed; may be null: nothing ever fills.
    TotalsFeed(plugins::ClientApi* api, Timing timing);
    explicit TotalsFeed(plugins::ClientApi* api) : TotalsFeed(api, Timing{}) {}
    ~TotalsFeed();

    TotalsFeed(const TotalsFeed&) = delete;
    TotalsFeed& operator=(const TotalsFeed&) = delete;

    // The host's repaint request (HostUi), called after new data lands --
    // outside the feed's lock. Optional: null (the default) means the data
    // shows at the next frame something else causes.
    void SetHostUi(plugins::HostUi* ui) { ui_.store(ui, std::memory_order_release); }

    // The cached string for the session, "" when nothing is known. Also the
    // focus hint: a changed session id wakes the fallback poller at once.
    std::string Render(const std::string& focused_session_id, TotalsField field) const;

    // The focus hint, pushed (an Observer's OnFocusChanged): the poller
    // follows the focused session even while no segment is rendered.
    void SetFocus(const std::string& session_id) const;
    // Drops a closed tab's figures (an Observer's OnTabClosed).
    void Forget(const std::string& session_id);

private:
    struct Entry {
        std::string cost;
        std::string detail;
        bool stale = false;
    };

    void RunLoop();
    bool OnPayload(const std::string& json_payload);
    void PollFallback();
    void ApplyLocked(const std::string& session_id, const nlohmann::json& totals);
    void RequestRedraw() const {
        if (auto* ui = ui_.load(std::memory_order_acquire)) ui->RequestRedraw();
    }
    std::optional<nlohmann::json> Fetch(const std::string& session_id) const;

    plugins::ClientApi* api_;
    // Atomic: set after the constructor has already started the worker.
    std::atomic<plugins::HostUi*> ui_{nullptr};
    Timing timing_;

    // mtx_/cv_/stop_ live in BackgroundPoller (protected base members),
    // constructed before any member below -- see background_poller.hpp.
    // mutable: Render() is const and records the focus hint.
    mutable std::string focused_session_id_;
    mutable bool refresh_requested_ = false;
    std::map<std::string, Entry> cache_;
};

}  // namespace minimal::client::cost_view
