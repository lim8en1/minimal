#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/common/logger.hpp"

using namespace minimal;
using namespace std::chrono_literals;

namespace {

class RecordingObserver : public agent::SessionObserver {
public:
    void OnSessionCreated(const agent::SessionInfo& info) override {
        created.push_back(info.session_id);
    }
    void OnSessionEnded(const agent::SessionInfo& info) override {
        ended.push_back(info.session_id);
    }
    void OnTurnCompleted(const agent::SessionInfo& info, const agent::TurnSummary& summary) override {
        turns.push_back({info.session_id, summary.stop_reason});
    }
    void OnToolExecuted(const agent::SessionInfo&, const agent::ToolInvocation& invocation) override {
        tools.push_back(invocation.tool_name);
    }

    std::vector<std::string> created, ended, tools;
    std::vector<std::pair<std::string, llm::StopReason>> turns;
};

class ThrowingObserver : public agent::SessionObserver {
public:
    void OnSessionEnded(const agent::SessionInfo&) override {
        called = true;
        throw std::runtime_error("observer blew up");
    }
    std::atomic<bool> called{false};
};

// Blocks forever. Models a plugin that wedges on a network call with no timeout.
class HangingObserver : public agent::SessionObserver {
public:
    void OnSessionEnded(const agent::SessionInfo&) override {
        entered.store(true);
        while (!release.load()) std::this_thread::sleep_for(1ms);
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
};

agent::SessionInfo MakeInfo(std::string id) {
    agent::SessionInfo info;
    info.session_id = std::move(id);
    info.name = "test";
    return info;
}

}  // namespace

TEST_CASE("observers receive every lifecycle event", "[session_observer]") {
    agent::SessionObserverRegistry registry(common::DefaultLogger());
    auto observer = std::make_shared<RecordingObserver>();
    registry.Register(observer);

    registry.NotifySessionCreated(MakeInfo("s1"));
    agent::TurnSummary summary;
    summary.stop_reason = llm::StopReason::EndTurn;
    registry.NotifyTurnCompleted(MakeInfo("s1"), summary);
    agent::ToolInvocation invocation;
    invocation.tool_name = "bash";
    registry.NotifyToolExecuted(MakeInfo("s1"), invocation);
    registry.NotifySessionEnded(MakeInfo("s1"));

    registry.WaitForIdleForTesting();
    CHECK(observer->created == std::vector<std::string>{"s1"});
    CHECK(observer->ended == std::vector<std::string>{"s1"});
    CHECK(observer->tools == std::vector<std::string>{"bash"});
    REQUIRE(observer->turns.size() == 1);
    CHECK(observer->turns[0].second == llm::StopReason::EndTurn);
}

TEST_CASE("a throwing observer does not stop the others", "[session_observer]") {
    // Observers are plugin code, so a throw is expected rather than exceptional.
    // If it escaped into the dispatch thread it would take down every future
    // notification, not just this one.
    agent::SessionObserverRegistry registry(common::DefaultLogger());
    auto thrower = std::make_shared<ThrowingObserver>();
    auto recorder = std::make_shared<RecordingObserver>();
    registry.Register(thrower);
    registry.Register(recorder);

    registry.NotifySessionEnded(MakeInfo("s1"));
    registry.WaitForIdleForTesting();

    CHECK(thrower->called.load());
    CHECK(recorder->ended == std::vector<std::string>{"s1"});

    // And the registry still works afterwards -- the dispatch thread survived.
    registry.NotifySessionEnded(MakeInfo("s2"));
    registry.WaitForIdleForTesting();
    CHECK(recorder->ended.size() == 2);
}

TEST_CASE("a hung observer cannot block the caller", "[session_observer]") {
    // The hard constraint: observers must never wedge a turn. Dispatch is
    // fire-and-forget, so this must return promptly even though the observer
    // never will. With a single ordered dispatch thread the cost of a hung
    // observer is that FURTHER notifications stall -- contained, and the price
    // of guaranteeing observers see events in order.
    auto hanger = std::make_shared<HangingObserver>();
    {
        agent::SessionObserverRegistry registry(common::DefaultLogger());
        registry.Register(hanger);

        const auto started = std::chrono::steady_clock::now();
        registry.NotifySessionEnded(MakeInfo("s1"));
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed < 200ms);

        while (!hanger->entered.load()) std::this_thread::sleep_for(1ms);
        // Released before teardown so the dispatcher can join; production has no
        // such luxury, which is why a hung observer is a real (if contained)
        // failure mode rather than a hypothetical one.
        hanger->release.store(true);
    }
    CHECK(hanger->entered.load());
}

TEST_CASE("notifications are delivered to every registered observer", "[session_observer]") {
    agent::SessionObserverRegistry registry(common::DefaultLogger());
    auto first = std::make_shared<RecordingObserver>();
    auto second = std::make_shared<RecordingObserver>();
    registry.Register(first);
    registry.Register(second);
    CHECK(registry.observer_count() == 2);

    registry.NotifySessionCreated(MakeInfo("s1"));
    registry.WaitForIdleForTesting();

    CHECK(first->created.size() == 1);
    CHECK(second->created.size() == 1);
}

TEST_CASE("with no observers registered, notifying is a no-op", "[session_observer]") {
    agent::SessionObserverRegistry registry(common::DefaultLogger());
    registry.NotifySessionCreated(MakeInfo("s1"));
    registry.NotifySessionEnded(MakeInfo("s1"));
    registry.WaitForIdleForTesting();
    CHECK(registry.observer_count() == 0);
}

TEST_CASE("queued notifications still run during shutdown", "[session_observer]") {
    // OnSessionEnded at process shutdown is precisely the notification a plugin
    // needs in order to release per-session state. Dropping the backlog on
    // destruction would silently skip it.
    auto observer = std::make_shared<RecordingObserver>();
    {
        agent::SessionObserverRegistry registry(common::DefaultLogger());
        registry.Register(observer);
        for (int i = 0; i < 50; ++i) {
            registry.NotifySessionEnded(MakeInfo("s" + std::to_string(i)));
        }
    }  // destructor drains, then joins
    CHECK(observer->ended.size() == 50);
}

TEST_CASE("Shutdown drains, then releases the observers before the registry dies",
          "[session_observer]") {
    // The host calls Shutdown while the services observers reach are still
    // alive; an observer's destructor (a plugin joining its own worker) must
    // therefore run inside Shutdown, not later in ~SessionObserverRegistry.
    agent::SessionObserverRegistry registry(common::DefaultLogger());
    auto observer = std::make_shared<RecordingObserver>();
    std::weak_ptr<RecordingObserver> weak = observer;
    registry.Register(observer);
    for (int i = 0; i < 10; ++i) {
        registry.NotifySessionEnded(MakeInfo("s" + std::to_string(i)));
    }
    observer.reset();  // the registry is now the only owner

    registry.Shutdown();
    CHECK(weak.expired());
    CHECK(registry.observer_count() == 0);

    // Idempotent, and later notifications are no-ops rather than crashes.
    registry.Shutdown();
    registry.NotifySessionEnded(MakeInfo("late"));
}

TEST_CASE("Shutdown delivers the queued backlog first", "[session_observer]") {
    agent::SessionObserverRegistry registry(common::DefaultLogger());
    auto observer = std::make_shared<RecordingObserver>();
    registry.Register(observer);
    for (int i = 0; i < 50; ++i) {
        registry.NotifySessionEnded(MakeInfo("s" + std::to_string(i)));
    }
    registry.Shutdown();
    CHECK(observer->ended.size() == 50);
}

TEST_CASE("a wedged observer drops notifications instead of blocking its dispatcher",
          "[session_observer][invariant]") {
    // THE CONTAINMENT PROPERTY, which the class and ARCHITECTURE.md both state
    // and a blocking Push contradicted: "a hung observer stalls further
    // notifications rather than leaking one pool thread -- contained (sessions
    // keep running; only notifications stop)."
    //
    // Dispatch used EventQueue::Push, which BLOCKS at the cap. NotifySessionCreated
    // runs inside Session's constructor -- reached from SessionRegistry::CreateResolved
    // on the gRPC HANDLER thread -- and NotifySessionEnded runs from Terminate.
    // So once a wedged observer filled the queue, session creation and teardown
    // blocked outright, including the one thread the three-thread topology
    // forbids from blocking. Which is the opposite of contained.
    constexpr std::size_t kQueue = 8;

    // The atomics are declared BEFORE the registry on purpose: destruction runs
    // in reverse, so the registry (whose destructor JOINS the wedged dispatcher
    // thread) goes away first and the thread is provably no longer touching
    // them when they go. The other order is a stack-use-after-scope at teardown
    // -- real, and ASan's whole point -- when the dispatcher drains its queued
    // tasks after `release` is set but before the join.
    std::atomic<bool> release{false};
    std::atomic<int> entered{0};
    agent::SessionObserverRegistry registry(common::DefaultLogger(), kQueue);

    // Wedged: the dispatcher thread parks in the first task and never returns,
    // so nothing is ever drained and the queue fills and stays full.
    struct Wedged : agent::SessionObserver {
        std::atomic<bool>* release;
        std::atomic<int>* entered;
        void OnSessionCreated(const agent::SessionInfo&) override {
            entered->fetch_add(1);
            while (!release->load()) std::this_thread::yield();
        }
    };
    auto observer = std::make_shared<Wedged>();
    observer->release = &release;
    observer->entered = &entered;
    registry.Register(observer);

    registry.NotifySessionCreated(MakeInfo("s0"));
    while (entered.load() == 0) std::this_thread::yield();  // dispatcher is now stuck

    // Far more than the queue holds. Every one of these must RETURN -- the
    // assertion is simply that this loop finishes at all. Against the old
    // Push() it deadlocks on the ninth, which is what the deadline below turns
    // into a failure rather than a hung suite.
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) registry.NotifySessionCreated(MakeInfo("s" + std::to_string(i)));
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(elapsed < std::chrono::seconds(5));
    CHECK(registry.dropped_notifications() > 0);  // and it says so rather than losing them silently

    release.store(true);
}
