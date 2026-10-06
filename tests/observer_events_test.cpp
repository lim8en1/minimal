// Plugin observers (observer_events.hpp): the events derived from successive
// tab tables, and their contained delivery.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "minimal/client/plugins/observer_events.hpp"
#include "minimal/client/state.hpp"

using minimal::client::AppState;
using minimal::client::SessionState;
namespace plugins = minimal::client::plugins;
using Kind = plugins::ObserverEvent::Kind;

namespace {

AppState::TabSnapshot Tab(std::string tab_id, std::string session_id, bool focused = false) {
    AppState::TabSnapshot snap;
    snap.tab_id = std::move(tab_id);
    snap.session_id = std::move(session_id);
    snap.display_name = "name";
    snap.is_focused = focused;
    return snap;
}

std::vector<Kind> Kinds(const std::vector<plugins::ObserverEvent>& events) {
    std::vector<Kind> out;
    for (const auto& event : events) out.push_back(event.kind);
    return out;
}

// Records every call as "<event>:<tab_id>".
class Recorder final : public plugins::Observer {
public:
    explicit Recorder(std::vector<std::string>& log) : log_(log) {}
    void OnTabOpened(const plugins::TabInfo& tab) override { log_.push_back("open:" + tab.tab_id); }
    void OnTabClosed(const plugins::TabInfo& tab) override { log_.push_back("close:" + tab.tab_id); }
    void OnFocusChanged(const plugins::TabInfo& tab) override {
        log_.push_back("focus:" + tab.tab_id);
    }

private:
    std::vector<std::string>& log_;
};

class Thrower final : public plugins::Observer {
public:
    explicit Thrower(int& calls) : calls_(calls) {}
    void OnFocusChanged(const plugins::TabInfo&) override {
        ++calls_;
        throw std::runtime_error("boom");
    }

private:
    int& calls_;
};

}  // namespace

TEST_CASE("observer events: open, focus, close, in order", "[observer_events]") {
    plugins::ObserverEventDeriver deriver;
    CHECK(deriver.Derive({}).empty());   // nothing open, nothing focused: quiet

    auto events = deriver.Derive({Tab("t1", "", /*focused=*/true), Tab("t2", "s2")});
    CHECK(Kinds(events) == std::vector<Kind>{Kind::TabOpened, Kind::TabOpened,
                                             Kind::SessionMetadata, Kind::FocusChanged});
    CHECK(events.back().tab.tab_id == "t1");

    // Unchanged table: no events.
    CHECK(deriver.Derive({Tab("t1", "", true), Tab("t2", "s2")}).empty());

    // The focused tab gets its session: a focus change (same tab, new session).
    events = deriver.Derive({Tab("t1", "s1", true), Tab("t2", "s2")});
    CHECK(Kinds(events) == std::vector<Kind>{Kind::SessionMetadata, Kind::FocusChanged});
    CHECK(events.back().tab.session_id == "s1");

    // t1 closes, focus moves to t2.
    events = deriver.Derive({Tab("t2", "s2", true)});
    CHECK(Kinds(events) == std::vector<Kind>{Kind::TabClosed, Kind::FocusChanged});
    CHECK(events.front().tab.session_id == "s1");   // as last seen
    CHECK(events.back().tab.tab_id == "t2");
}

TEST_CASE("observer events: a turn inside one frame reports both edges", "[observer_events]") {
    plugins::ObserverEventDeriver deriver;
    auto tab = Tab("t1", "s1", true);
    deriver.Derive({tab});

    tab.turns_started = 1;
    tab.turns_ended = 1;
    tab.last_turn_completed = false;
    const auto events = deriver.Derive({tab});
    REQUIRE(Kinds(events) == std::vector<Kind>{Kind::TurnStarted, Kind::TurnCompleted});
    CHECK_FALSE(events.back().completed);

    tab.turns_started = 2;   // the next one starts...
    CHECK(Kinds(deriver.Derive({tab})) == std::vector<Kind>{Kind::TurnStarted});
    tab.turns_ended = 2;     // ...and completes
    tab.last_turn_completed = true;
    const auto done = deriver.Derive({tab});
    REQUIRE(Kinds(done) == std::vector<Kind>{Kind::TurnCompleted});
    CHECK(done.front().completed);
}

TEST_CASE("observer events: metadata reports merged changes only", "[observer_events]") {
    plugins::ObserverEventDeriver deriver;
    auto tab = Tab("t1", "s1", true);
    tab.model = "m1";
    auto events = deriver.Derive({tab});
    REQUIRE(Kinds(events) ==
            std::vector<Kind>{Kind::TabOpened, Kind::SessionMetadata, Kind::FocusChanged});
    CHECK(events[1].metadata.model == "m1");

    tab.model = "m2";
    events = deriver.Derive({tab});
    REQUIRE(Kinds(events) == std::vector<Kind>{Kind::SessionMetadata});
    CHECK(events.front().metadata.model == "m2");
}

TEST_CASE("observer delivery: in order, a thrower is contained and disabled",
          "[observer_events]") {
    plugins::ObserverRegistry registry;
    std::vector<std::string> log;
    int throws = 0;
    registry.Register(std::make_unique<Thrower>(throws));
    registry.Register(std::make_unique<Recorder>(log));
    plugins::ObserverDispatcher dispatcher(registry);

    plugins::ObserverEventDeriver deriver;
    dispatcher.Deliver(deriver.Derive({Tab("t1", "", true), Tab("t2", "")}));
    CHECK(log == std::vector<std::string>{"open:t1", "open:t2", "focus:t1"});

    for (int i = 0; i < 2 * minimal::client::kMaxPluginThrows; ++i) {
        dispatcher.Deliver({{Kind::FocusChanged, {"t2", "", ""}}});
    }
    // Every delivery reached the healthy observer; the thrower stopped being
    // called once disabled.
    CHECK(throws == minimal::client::kMaxPluginThrows);
    CHECK(log.size() == 3 + 2 * minimal::client::kMaxPluginThrows);
}

TEST_CASE("session turn counters count each edge once", "[observer_events]") {
    SessionState state("s1");
    state.OnChatSubmitted("hi");
    state.OnChatSubmitted("queued");   // still the same turn in flight
    auto snap = state.TakeLightSnapshot(false);
    CHECK(snap.turns_started == 1);
    CHECK(snap.turns_ended == 0);

    state.OnError("dead stream");
    state.OnError("again");   // nothing in flight: no second edge
    snap = state.TakeLightSnapshot(false);
    CHECK(snap.turns_ended == 1);
    CHECK_FALSE(snap.last_turn_completed);

    state.OnChatSubmitted("retry");
    state.OnTurnCompleted({});
    snap = state.TakeLightSnapshot(false);
    CHECK(snap.turns_started == 2);
    CHECK(snap.turns_ended == 2);
    CHECK(snap.last_turn_completed);
}
