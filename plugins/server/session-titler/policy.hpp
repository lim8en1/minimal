#pragma once

// session-titler policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::session_titler {

// Titling jobs queued while the model answers: one per new session, so a
// few dozen pending means titling has stalled, not that work is waiting.
inline constexpr std::size_t kJobQueueSize = 64;

// The titling call: a title is a handful of words, so a few dozen output
// tokens; the first messages it reads are cut to this many characters --
// enough to see what the session is about.
inline constexpr int kDefaultMaxTokens = 64;
inline constexpr int kDefaultInputCharBudget = 2000;

}  // namespace minimal::session_titler
