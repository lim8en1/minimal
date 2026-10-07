#pragma once

// chat-renderers policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::client::chat_renderers {

// Long row content renders in chunks of at most this many source lines, so a
// streamed delta re-renders one small chunk, not the whole row.
inline constexpr std::size_t kMaxAnchorLines = 20;

// An expanded attachment renders at most this much: an accidentally expanded
// huge file must not wedge the renderer.
inline constexpr std::size_t kAttachmentExpandedRenderCapChars = 200'000;

// A tool result's body shown on its row (the tail window): orientation, not
// an archive -- the persisted result is the record.
inline constexpr std::size_t kToolResultBodyPreviewBytes = 4000;

}  // namespace minimal::client::chat_renderers
