#pragma once

// Core's widget kit, for tests that build plugin views and modals directly
// (a running TUI hands plugins the host's kit through the registration
// context; a test has no host).

#include "minimal/client/plugins/core_registrations.hpp"

namespace minimal_test {

inline const minimal::client::plugins::WidgetKit& CoreWidgetKit() {
    static const minimal::client::plugins::WidgetKitRegistry* registry = [] {
        auto* slot = new minimal::client::plugins::WidgetKitRegistry;  // lives for the run
        minimal::client::plugins::RegisterDefaultWidgetKit(*slot);
        return slot;
    }();
    return *registry->get();
}

}  // namespace minimal_test
