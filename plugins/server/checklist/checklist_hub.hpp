#pragma once

// The push side of the checklist data path: every live PluginSubscribe
// "changes" stream (checklist-view's segment and panel) is a sink here, and
// the tool marks a session dirty after every successful write. A worker
// coalesces the dirty set, loads each dirty session's list ONCE, and pushes
// it to every sink -- replacing a per-client 3 s poll of get + list.
//
// WIRE (JSON, one object per push):
//   {"type": "checklist", "session_id": ..., "items": [{id,text,status}]}
//                                    items [] = cleared (authoritative)
//   {"type": "heartbeat"}            every heartbeat interval while idle
// The stream is session-agnostic -- the subscriber filters -- so a tab switch
// never needs a new subscription.
//
// HEARTBEATS ARE LOAD-BEARING: the client can only end a subscription from
// its payload callback, so an idle stream would pin the subscriber's
// teardown until the model next touched a list. The heartbeat bounds it.
//
// THREADING/LIFETIME: MarkDirty is a set insert and a notify (the tool never
// waits on subscribers). The worker starts on the first AddSink, so a server
// nobody subscribes to runs no thread. Shutdown() joins it and must run
// before PluginStorage goes away: ChecklistHubOwner calls it from the
// observer registry's teardown (cost-tracker's TotalsHubOwner pattern).

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "checklist_store.hpp"
#include "minimal/agent/session_observer.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace minimal::checklist {

class ChecklistHub {
public:
    // `store` is borrowed; process lifetime.
    explicit ChecklistHub(ChecklistStore& store,
                          std::chrono::milliseconds heartbeat = std::chrono::milliseconds(500),
                          std::chrono::milliseconds debounce = std::chrono::milliseconds(50));
    ~ChecklistHub();

    ChecklistHub(const ChecklistHub&) = delete;
    ChecklistHub& operator=(const ChecklistHub&) = delete;

    void AddSink(std::shared_ptr<plugins::PluginSubscriptionSink> sink);

    // Cheap, and a no-op while nobody is subscribed.
    void MarkDirty(const std::string& session_id);

    // Idempotent. Later AddSink calls are refused (the sink is dropped).
    void Shutdown();

private:
    void Run();

    ChecklistStore& store_;
    std::chrono::milliseconds heartbeat_;
    std::chrono::milliseconds debounce_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::set<std::string> dirty_;
    std::vector<std::shared_ptr<plugins::PluginSubscriptionSink>> sinks_;
    std::thread worker_;
};

// All-default observer whose destructor is the hub's teardown hook.
class ChecklistHubOwner : public agent::SessionObserver {
public:
    explicit ChecklistHubOwner(ChecklistHub& hub) : hub_(hub) {}
    ~ChecklistHubOwner() override { hub_.Shutdown(); }

private:
    ChecklistHub& hub_;
};

}  // namespace minimal::checklist
