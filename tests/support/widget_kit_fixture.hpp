#pragma once

// The bundled widget kit (plugins/client/ui-kit), for tests that build plugin
// views and modals directly: a running TUI hands plugins the host's proxy
// over the kit slot through the registration context; a test has no host.

#include "ui_kit.hpp"

namespace minimal_test {

inline const minimal::client::plugins::WidgetKit& CoreWidgetKit() {
    static const minimal::client::ui_kit::UiKit kit;
    return kit;
}

}  // namespace minimal_test
