#pragma once

// The kit's modal chrome -- the window every one of its widgets sits in:
// titled, framed in the attention color, its body in the body color and
// scrolling under the screen's height, details pinned below the body and the
// key hints pinned last. (Client core keeps its own copy for the permission
// and confirmation prompts until those are prompt renderers too.)

#include <string>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/policy.hpp"
#include "minimal/client/render/theme.hpp"

namespace minimal::client::ui_kit {

// A divider between sections of a modal's body.
ftxui::Element ModalRule(const Theme& theme);

// `rows` scroll within the screen's height; `pinned` (when not empty) stays
// visible below them, at most kModalPinnedMaxRows rows; `footer` (the key
// hints, when not empty) is pinned last, wrapped rather than clipped.
ftxui::Element ModalFrame(const Theme& theme, const std::string& title, ftxui::Elements rows,
                          ftxui::Elements pinned, const std::string& footer,
                          int max_width = kModalMaxWidth);

}  // namespace minimal::client::ui_kit
