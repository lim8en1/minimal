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

#include <chrono>
#include <map>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/background_poller.hpp"
#include "minimal/client/plugins/registries.hpp"

namespace minimal::client::cost_view {

namespace plugins = minimal::client::plugins;

enum class TotalsField { kCost, kTokenDetail };

class TotalsFeed : private plugins::BackgroundPoller {
public:
    struct Timing {
        std::chrono::milliseconds poll_interval{std::chrono::seconds(3)};
        std::chrono::milliseconds fallback_period{std::chrono::seconds(30)};
    };

    // Borrowed; either may be null. No ClientApi: nothing ever fills. No
    // FocusProvider: cached entries for closed tabs are never evicted (a
    // few short strings per session seen -- harmless, just unbounded).
    TotalsFeed(plugins::ClientApi* api, plugins::FocusProvider* focus, Timing timing);
    explicit TotalsFeed(plugins::ClientApi* api, plugins::FocusProvider* focus = nullptr)
        : TotalsFeed(api, focus, Timing{}) {}
    ~TotalsFeed();

    TotalsFeed(const TotalsFeed&) = delete;
    TotalsFeed& operator=(const TotalsFeed&) = delete;

    // The cached string for the session, "" when nothing is known. Also the
    // focus hint: a changed session id wakes the fallback poller at once.
    std::string Render(const std::string& focused_session_id, TotalsField field) const;

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
    void EvictClosedTabs();
    std::optional<nlohmann::json> Fetch(const std::string& session_id) const;

    plugins::ClientApi* api_;
    plugins::FocusProvider* focus_;
    Timing timing_;

    // mtx_/cv_/stop_ live in BackgroundPoller (protected base members),
    // constructed before any member below -- see background_poller.hpp.
    // mutable: Render() is const and records the focus hint.
    mutable std::string focused_session_id_;
    mutable bool refresh_requested_ = false;
    std::map<std::string, Entry> cache_;
};

}  // namespace minimal::client::cost_view
