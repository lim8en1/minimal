#pragma once

// policy-module policy (pattern matching, atomic writes): every value here is
// a judgment call, not a fact or a derivation, and states why it is what it
// is. See ARCHITECTURE.md "Policy constants". Named match_policy.hpp because
// this module shares the minimal/tools/ include prefix with the tools module.

#include <cstddef>

namespace minimal::tools {

// Compiled glob patterns cached per thread. Permission rules are a few dozen
// patterns; a process matching an unbounded stream of distinct ones starts the
// cache over instead of growing.
inline constexpr std::size_t kMaxCachedGlobs = 2048;

}  // namespace minimal::tools
