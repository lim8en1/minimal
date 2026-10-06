#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>

namespace minimal::common {

// Wall-clock milliseconds since the Unix epoch: the timestamp unit every
// persisted record (sessions, plugin storage, memory, ULIDs) uses.
inline std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Whole seconds in a millisecond count (truncating toward zero), and back.
// For int64 fields and config keys that carry a duration in one unit or the
// other; a value that is already a std::chrono duration just converts.
constexpr std::int64_t MillisToSeconds(std::int64_t ms) {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds(ms))
        .count();
}
constexpr std::int64_t SecondsToMillis(std::int64_t seconds) {
    return std::chrono::milliseconds(std::chrono::seconds(seconds)).count();
}

// A Unix-epoch millisecond timestamp (NowMillis' unit) as a time_t, for
// gmtime_r/localtime_r.
inline std::time_t UnixMillisToTimeT(std::int64_t ms) {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(ms))));
}

}  // namespace minimal::common
