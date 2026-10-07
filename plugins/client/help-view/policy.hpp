#pragma once

// help-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::help_view {

// The keymap help's key column: room for "ctrl+alt+shift+x"; a longer spec
// is clipped rather than pushing the descriptions off screen.
inline constexpr std::size_t kKeyColumnCap = 18;

}  // namespace minimal::help_view
