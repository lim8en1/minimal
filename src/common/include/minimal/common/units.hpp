#pragma once

// Binary byte multiples, and percentages. A size is written as `count * unit`
// (`64 * kMiB`):
// the count is the judgment call (policy, ARCHITECTURE.md "Policy
// constants"), the unit is a fact. Durations use std::chrono, never a bare
// 1000 or 3600.

#include <cstddef>
#include <cstdint>

// NOLINTBEGIN(readability-magic-numbers) -- the units themselves.

namespace minimal::common {

inline constexpr std::size_t kKiB = std::size_t{1} << 10;
inline constexpr std::size_t kMiB = std::size_t{1} << 20;
inline constexpr std::size_t kGiB = std::size_t{1} << 30;

// `percent` percent of `total`, rounding toward zero.
constexpr std::int64_t PercentOf(std::int64_t total, std::int64_t percent) {
    return total * percent / 100;
}

// `part` as a percentage of `total` (> 0), rounding toward zero.
constexpr std::int64_t AsPercent(std::int64_t part, std::int64_t total) {
    return part * 100 / total;
}

}  // namespace minimal::common

// NOLINTEND(readability-magic-numbers)
