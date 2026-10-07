#pragma once

// session-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::session_view {

// The session picker's message-count column ("12345 msgs").
inline constexpr std::size_t kMessageCountColumnWidth = 9;
// An unnamed subagent is labelled by its session id's prefix: the ULID's
// timestamp part plus a little randomness, enough to tell siblings apart.
inline constexpr std::size_t kUnnamedSessionIdPrefixChars = 12;

}  // namespace minimal::session_view
