#pragma once

// The host side of plugin observers (registries.hpp Observer): typed events
// derived by diffing the tab table between frames, delivered with plugin-throw
// containment. UI thread only.
//
// Derived, not pushed from where things happen: focus moves along half a
// dozen paths (a click, a close, a first tab, a session arriving on the
// focused tab), and every one of them is already reflected in the tab table.
// Turns are counted, not sampled (SessionState::Snapshot::turns_started /
// turns_ended), so a turn that starts and ends inside one frame still reports
// both edges.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client::plugins {

struct ObserverEvent {
    enum class Kind { TabOpened, TabClosed, FocusChanged, TurnStarted, TurnCompleted, SessionMetadata };
    Kind kind = Kind::TabOpened;
    TabInfo tab;
    bool completed = true;        // TurnCompleted only
    SessionMetadata metadata;     // SessionMetadata only
};

// Pure: the events that turn the table it last saw into `tabs`. Per tab, in
// table order: opened, turn edges, metadata; then closes; then focus.
class ObserverEventDeriver {
public:
    std::vector<ObserverEvent> Derive(const std::vector<AppState::TabSnapshot>& tabs);

private:
    struct Seen {
        TabInfo info;
        std::uint64_t turns_started = 0;
        std::uint64_t turns_ended = 0;
        SessionMetadata metadata;
    };
    std::map<std::string, Seen> seen_;   // by tab_id
    TabInfo focused_;
};

// Calls every observer for each event, in order. A throw is caught and
// logged; an observer that throws kMaxPluginThrows times is disabled for the
// process.
class ObserverDispatcher {
public:
    explicit ObserverDispatcher(const ObserverRegistry& registry) : registry_(registry) {}
    void Deliver(const std::vector<ObserverEvent>& events);

private:
    const ObserverRegistry& registry_;
    std::map<const Observer*, int> throws_;
};

// Both, over AppState: what the frame loop runs between frames. Free when no
// plugin registered an observer, and when the app state has not moved.
class ObserverPump {
public:
    ObserverPump(const AppState& app, const ObserverRegistry& registry)
        : app_(app), registry_(registry), dispatcher_(registry) {}
    void Run();

private:
    const AppState& app_;
    const ObserverRegistry& registry_;
    ObserverEventDeriver deriver_;
    ObserverDispatcher dispatcher_;
    bool ran_ = false;
    std::uint64_t last_version_ = 0;
};

}  // namespace minimal::client::plugins
