#pragma once

// The push side of the totals data path: every live PluginSubscribe
// "totals" stream (cost-view's status segments) is a sink here, and the
// observer marks sessions dirty as rounds and turns land. A worker thread
// coalesces the dirty set, recomputes each dirty session's subtree totals
// ONCE, and pushes them to every sink -- replacing a per-client 3 s poll
// whose every call walked the whole store.
//
// WIRE (JSON, one object per push):
//   {"type": "totals", "session_id": ..., <the "totals" RPC body>}
//   {"type": "totals", "session_id": ..., "turns": 0}   nothing recorded
//                                                       (e.g. rewound away)
//   {"type": "heartbeat"}                               every heartbeat
//                                                       interval while idle
// Dirtying a session dirties its whole ancestor chain: a subagent's spend
// is part of every ancestor's roll-up. The stream is session-agnostic -- the
// subscriber filters -- so a tab switch never needs a new subscription.
//
// HEARTBEATS ARE LOAD-BEARING: the client can only end a subscription from
// its payload callback, so an idle stream with nothing to push would pin the
// subscriber's teardown until the next turn. The heartbeat bounds it.
//
// THREADING/LIFETIME: MarkDirty never blocks on the store (the observer
// thread's "never wedge a turn" contract) -- ancestor expansion and the
// recompute happen on the worker. The worker starts on the first AddSink,
// so a server nobody subscribes to runs no thread. Shutdown() joins it and
// must run before PluginStorage goes away: TotalsHubOwner (the
// RatesRefresherOwner pattern) calls it from the observer registry's
// teardown.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/session_observer.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "session_index.hpp"

namespace minimal::cost_tracker {

class TotalsHub {
public:
    // The "totals" body for a session, or nullopt when nothing is recorded.
    // May throw; a throw skips that session for this round.
    using Compute = std::function<std::optional<nlohmann::json>(const std::string& session_id)>;

    TotalsHub(SessionIndex& index, Compute compute,
              std::chrono::milliseconds heartbeat = std::chrono::milliseconds(500),
              std::chrono::milliseconds debounce = std::chrono::milliseconds(200));
    ~TotalsHub();

    TotalsHub(const TotalsHub&) = delete;
    TotalsHub& operator=(const TotalsHub&) = delete;

    void AddSink(std::shared_ptr<plugins::PluginSubscriptionSink> sink);

    // Observer-thread safe and cheap: a set insert and a notify, and a no-op
    // while nobody is subscribed.
    void MarkDirty(const std::string& session_id);

    // Idempotent. Later AddSink calls are refused (the sink is dropped).
    void Shutdown();

private:
    void Run();

    SessionIndex& index_;
    Compute compute_;
    std::chrono::milliseconds heartbeat_;
    std::chrono::milliseconds debounce_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::set<std::string> dirty_;
    std::vector<std::shared_ptr<plugins::PluginSubscriptionSink>> sinks_;
    std::thread worker_;
};

// All-default observer whose destructor is the hub's teardown hook -- the
// RatesRefresherOwner pattern (rates_refresh.hpp) for the same reason.
class TotalsHubOwner : public agent::SessionObserver {
public:
    explicit TotalsHubOwner(TotalsHub& hub) : hub_(hub) {}
    ~TotalsHubOwner() override { hub_.Shutdown(); }

private:
    TotalsHub& hub_;
};

}  // namespace minimal::cost_tracker
