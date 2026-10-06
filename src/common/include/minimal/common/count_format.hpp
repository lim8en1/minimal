#pragma once

// The one compact count format for token counts on screen (status bar, row
// headlines, the cost segment): "950", "4.2k", "62k", "1.3M", "2M", "1.2G".
// Exact counts (a table's "1,234,567") are a different format, not this one.

#include <cstdint>
#include <string>

namespace minimal::common {

// One decimal below ten of a unit ("4.2k"), none above ("62k"); a trailing
// ".0" is dropped ("2M", not "2.0M"). Truncates, never rounds up into the
// next unit.
// NOLINTBEGIN(readability-magic-numbers) -- decimal SI scaling.
inline std::string CompactCount(std::int64_t n) {
    const auto scaled = [](std::int64_t value, std::int64_t unit, const char* suffix) {
        if (value < 10 * unit) {
            const std::int64_t tenths = value * 10 / unit;
            std::string out = std::to_string(tenths / 10);
            if (tenths % 10 != 0) out += "." + std::to_string(tenths % 10);
            return out + suffix;
        }
        return std::to_string(value / unit) + suffix;
    };
    if (n < 1'000) return std::to_string(n);
    if (n < 1'000'000) return scaled(n, 1'000, "k");
    if (n < 1'000'000'000) return scaled(n, 1'000'000, "M");
    return scaled(n, 1'000'000'000, "G");
}
// NOLINTEND(readability-magic-numbers)

}  // namespace minimal::common
