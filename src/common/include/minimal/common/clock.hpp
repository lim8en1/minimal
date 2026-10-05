#pragma once

#include <chrono>
#include <cstdint>

namespace minimal::common {

// Wall-clock milliseconds since the Unix epoch: the timestamp unit every
// persisted record (sessions, plugin storage, memory, ULIDs) uses.
inline std::int64_t NowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace minimal::common
