#include "minimal/client/plugins/observer_events.hpp"

#include <exception>
#include <set>

#include "minimal/common/logger.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client::plugins {

namespace {

bool SameTab(const TabInfo& a, const TabInfo& b) {
    return a.tab_id == b.tab_id && a.session_id == b.session_id;
}

}  // namespace

std::vector<ObserverEvent> ObserverEventDeriver::Derive(
    const std::vector<AppState::TabSnapshot>& tabs) {
    using Kind = ObserverEvent::Kind;
    std::vector<ObserverEvent> events;
    std::set<std::string> present;
    TabInfo focused;

    for (const auto& snap : tabs) {
        const TabInfo info{snap.tab_id, snap.session_id, snap.display_name};
        present.insert(snap.tab_id);
        if (snap.is_focused) focused = info;

        auto [it, opened] = seen_.try_emplace(snap.tab_id);
        Seen& seen = it->second;
        seen.info = info;
        if (opened) events.push_back({Kind::TabOpened, info});

        // Turns are strictly sequential (a session never starts one before
        // the last ended), so the edges replay in order. Only the LAST turn's
        // outcome is known; two turns inside one frame both report it.
        for (;;) {
            if (seen.turns_started == seen.turns_ended && seen.turns_started < snap.turns_started) {
                events.push_back({Kind::TurnStarted, info});
                ++seen.turns_started;
            } else if (seen.turns_ended < seen.turns_started &&
                       seen.turns_ended < snap.turns_ended) {
                events.push_back({Kind::TurnCompleted, info, snap.last_turn_completed});
                ++seen.turns_ended;
            } else {
                break;
            }
        }
        seen.turns_started = snap.turns_started;
        seen.turns_ended = snap.turns_ended;

        if (!snap.session_id.empty()) {
            const SessionMetadata metadata{snap.display_name, snap.model, snap.agent_preset,
                                           snap.permission_mode};
            if (metadata != seen.metadata) {
                seen.metadata = metadata;
                events.push_back({Kind::SessionMetadata, info, true, metadata});
            }
        }
    }

    for (auto it = seen_.begin(); it != seen_.end();) {
        if (present.count(it->first) != 0) {
            ++it;
            continue;
        }
        events.push_back({Kind::TabClosed, it->second.info});
        it = seen_.erase(it);
    }

    if (!SameTab(focused, focused_)) {
        focused_ = focused;
        events.push_back({Kind::FocusChanged, focused});
    }
    return events;
}

void ObserverDispatcher::Deliver(const std::vector<ObserverEvent>& events) {
    using Kind = ObserverEvent::Kind;
    for (const auto& observer : registry_.All()) {
        int& throws = throws_[observer.get()];
        for (const auto& event : events) {
            if (throws >= kMaxPluginThrows) break;
            try {
                switch (event.kind) {
                    case Kind::TabOpened:       observer->OnTabOpened(event.tab); break;
                    case Kind::TabClosed:       observer->OnTabClosed(event.tab); break;
                    case Kind::FocusChanged:    observer->OnFocusChanged(event.tab); break;
                    case Kind::TurnStarted:     observer->OnTurnStarted(event.tab); break;
                    case Kind::TurnCompleted:
                        observer->OnTurnCompleted(event.tab, event.completed);
                        break;
                    case Kind::SessionMetadata:
                        observer->OnSessionMetadata(event.tab, event.metadata);
                        break;
                }
            } catch (const std::exception& error) {
                ++throws;
                common::DefaultLogger().warn("plugin observer threw: {}{}", error.what(),
                                             throws >= kMaxPluginThrows ? " -- disabled" : "");
            } catch (...) {
                ++throws;
                common::DefaultLogger().warn("plugin observer threw a non-standard exception{}",
                                             throws >= kMaxPluginThrows ? " -- disabled" : "");
            }
        }
    }
}

void ObserverPump::Run() {
    if (registry_.All().empty()) return;
    const std::uint64_t version = app_.UiVersion();
    if (ran_ && version == last_version_) return;
    ran_ = true;
    last_version_ = version;
    const auto events = deriver_.Derive(app_.GetAllTabs(/*include_tool_output=*/false));
    if (!events.empty()) dispatcher_.Deliver(events);
}

}  // namespace minimal::client::plugins
