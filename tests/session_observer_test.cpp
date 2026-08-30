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
