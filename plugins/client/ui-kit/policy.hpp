#pragma once

// ui-kit policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::client::ui_kit {

// A picker menu's scrolling height.
inline constexpr int kMenuMaxHeight = 20;
// Modals: the default width (the body's hints get 78 columns inside a
// 2-column margin); rows reserved around a modal body (borders and the
// pinned hint line, plus slack for the tab bar and editor it overlays); a
// body always gets at least the minimum.
inline constexpr int kModalMaxWidth = 82;
inline constexpr int kModalFrameRows = 3;
inline constexpr int kModalOverlaySlackRows = 3;
// A window()'s top and bottom border: what a modal's inside height leaves
// out of the terminal's (ModalInnerHeight).
inline constexpr int kModalBorderRows = 2;
inline constexpr int kMinModalBodyRows = 2;
// A modal's pinned rows (ModalFrame's `pinned`: a selected row's details, the
// last outcome) take at most this many rows.
inline constexpr int kModalPinnedMaxRows = 8;
// The schema form (/config): a field's label and value columns -- room for a
// dotted key and a typical value; longer ones are clipped, the full value is
// one keypress away in the editor.
inline constexpr std::size_t kSchemaFormLabelWidth = 28;
inline constexpr std::size_t kSchemaFormValueWidth = 30;

}  // namespace minimal::client::ui_kit
