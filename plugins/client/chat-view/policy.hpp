#pragma once

// chat-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::client::chat_view {

// Live thinking text kept on screen while it streams: settled lines, and the
// trailing partial line's bytes ever re-wrapped. Presentation only; the
// transcript is authoritative.
inline constexpr std::size_t kMaxLiveThinkingLines = 300;
inline constexpr std::size_t kMaxLiveThinkingTailBytes = 64 * common::kKiB;
// Mouse wheel: events closer than the gate are dropped (about one step per
// two frames at a ~60 Hz repaint), and each accepted tick moves a few lines --
// small enough to read, large enough that a trackpad flick travels.
inline constexpr std::chrono::milliseconds kWheelGate{30};
inline constexpr int kWheelLinesPerTick = 3;
// Keyboard scroll step before the first layout pass has measured the viewport.
inline constexpr int kFallbackViewportLines = 20;

}  // namespace minimal::client::chat_view
