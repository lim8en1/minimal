#pragma once

// The bundled chat-view plugin's ChatView, driven from core's own snapshot:
// what the TUI does each frame (ChatFrameFrom, then the view), so the view
// tests keep building SessionState snapshots.

#include <vector>

#include "chat_view.hpp"

#include "minimal/client/state.hpp"
#include "minimal/client/view/chat_frame.hpp"

namespace minimal_test {

using minimal::client::chat_view::LiveTextStreamSlot;

class ChatViewHarness : public minimal::client::chat_view::ChatView {
public:
    using ChatView::ChatView;
    using ChatView::Render;
    ftxui::Element Render(const minimal::client::SessionState::Snapshot& snapshot,
                          const std::vector<LiveTextStreamSlot>& streams = {}) {
        return ChatView::Render(minimal::client::ChatFrameFrom(snapshot), streams);
    }
};

}  // namespace minimal_test
