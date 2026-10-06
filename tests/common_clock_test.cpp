// minimal/common/clock.hpp's unit conversions, and the client's one duration
// format built on std::chrono (minimal/client/view/tool_status.hpp).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <ctime>
#include <limits>

#include "minimal/client/view/tool_status.hpp"
#include "minimal/common/clock.hpp"

using minimal::common::MillisToSeconds;
using minimal::common::SecondsToMillis;
using minimal::common::UnixMillisToTimeT;

TEST_CASE("MillisToSeconds truncates; SecondsToMillis is exact", "[clock]") {
    CHECK(MillisToSeconds(0) == 0);
    CHECK(MillisToSeconds(999) == 0);
    CHECK(MillisToSeconds(1500) == 1);
    CHECK(SecondsToMillis(90) == 90'000);
    // The bash tool's overflow guard: the largest second count whose ms value
    // still fits.
    const auto max_seconds = MillisToSeconds(std::numeric_limits<std::int64_t>::max());
    CHECK(SecondsToMillis(max_seconds) <= std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("UnixMillisToTimeT drops the sub-second part", "[clock]") {
    CHECK(UnixMillisToTimeT(0) == 0);
    CHECK(UnixMillisToTimeT(1'700'000'000'999) == static_cast<std::time_t>(1'700'000'000));
}

TEST_CASE("FormatDuration: ms, tenths of a second, then minutes and seconds", "[clock]") {
    using minimal::client::FormatDuration;
    CHECK(FormatDuration(850) == "850ms");
    CHECK(FormatDuration(3'449) == "3.4s");
    CHECK(FormatDuration(59'999) == "59.9s");
    CHECK(FormatDuration(125'000) == "2m 5s");
}
