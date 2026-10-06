#pragma once

// Binary byte multiples. A size is written as `count * unit` (`64 * kMiB`):
// the count is the judgment call (policy, ARCHITECTURE.md "Policy
// constants"), the unit is a fact. Durations use std::chrono, never a bare
// 1000 or 3600.

#include <cstddef>

// NOLINTBEGIN(readability-magic-numbers) -- the units themselves.

namespace minimal::common {

inline constexpr std::size_t kKiB = std::size_t{1} << 10;
inline constexpr std::size_t kMiB = std::size_t{1} << 20;
inline constexpr std::size_t kGiB = std::size_t{1} << 30;

}  // namespace minimal::common

// NOLINTEND(readability-magic-numbers)
