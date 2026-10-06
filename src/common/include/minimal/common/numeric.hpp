#pragma once

// Numeric conversions with a range guard, shared by every place that narrows a
// double from the wire (protobuf Value numbers carry JSON integers as doubles).

#include <cmath>
#include <cstdint>
#include <limits>

namespace minimal::common {

// True when `value` is an integer an int64_t holds, so the cast is defined.
// STRICTLY LESS at the top, and the asymmetry is the point:
// static_cast<double>(INT64_MAX) is not INT64_MAX -- 2^63-1 is not
// representable, so it rounds UP to exactly 2^63, and a `<=` against that
// admits a value whose cast is UB. INT64_MIN is exactly -2^63 and needs no
// such care.
inline bool IsExactInt64(double value) {
    if (!std::isfinite(value)) return false;
    if (value != std::trunc(value)) return false;
    constexpr double kTwoToThe63 = -static_cast<double>(std::numeric_limits<std::int64_t>::min());
    return value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
           value < kTwoToThe63;
}

}  // namespace minimal::common
