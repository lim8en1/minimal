#pragma once

// tools policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

#include "minimal/common/units.hpp"

namespace minimal::tools {

// Bytes inspected to decide a file is binary: the head of a text file never
// holds a NUL, and a binary's header almost always does within this.
inline constexpr std::size_t kBinarySniffBytes = 8 * common::kKiB;

// One line's ceiling when no max_line_length is configured: the reader's
// memory tracks its limit, and "no truncation" must not mean "a whole file for
// one line". No real source line reaches it.
inline constexpr std::size_t kHardLineCapBytes = common::kMiB;

// Advisor threads launched and not yet exited. An abandoned one is harmless,
// but an advisor that wedges on every call would add one per Ask; past this
// the advisor is skipped and the human is asked directly.
inline constexpr int kMaxAbandonedAdvisors = 8;

// Spilled tool outputs kept per session (tool_output_spill.hpp). A model
// rarely reaches back past the last few cut outputs; the bound keeps a long
// session from filling the project's .minimal/ with logs nobody reads.
inline constexpr std::size_t kSpillKeepPerSession = 50;

}  // namespace minimal::tools
