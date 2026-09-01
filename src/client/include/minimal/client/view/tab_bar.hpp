#pragma once

// The tab bar renders IDENTITY and ATTENTION and nothing else (ARCHITECTURE.md
// "Tab states and attention"): the session name (truncated) with the active-tab
// highlight, state carried by COLOR -- which costs zero width -- and the one
// exception, a literal `!` for Waiting, the single state that demands human
// action and must not depend on color alone. No nesting glyphs, no metadata:
// adjacency conveys grouping; the status bar and tools panel carry detail.

#include <ftxui/dom/elements.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

ftxui::Element RenderTabBar(const std::vector<AppState::TabSnapshot>& tabs, const Theme& theme);

}  // namespace minimal::client
