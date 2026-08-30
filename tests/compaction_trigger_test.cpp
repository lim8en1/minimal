#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/compaction_trigger.hpp"

using minimal::config::CompactionTriggerSettings;

namespace {

CompactionTriggerSettings Never() { return {}; }

CompactionTriggerSettings Fixed(std::int64_t tokens) {
    CompactionTriggerSettings trigger;
    trigger.kind = CompactionTriggerSettings::Kind::FixedTokens;
    trigger.tokens = tokens;
    return trigger;
}

CompactionTriggerSettings Percent(int percent) {
    CompactionTriggerSettings trigger;
    trigger.kind = CompactionTriggerSettings::Kind::PercentOfWindow;
    trigger.percent = percent;
    return trigger;
}

CompactionTriggerSettings Buffer(std::int64_t buffer_tokens) {
    CompactionTriggerSettings trigger;
    trigger.kind = CompactionTriggerSettings::Kind::WindowBuffer;
    trigger.buffer_tokens = buffer_tokens;
    return trigger;
}

std::int64_t Resolve(const CompactionTriggerSettings& trigger,
                     std::optional<std::int64_t> window = std::nullopt) {
    return minimal::agent::resolve_compaction_trigger(trigger, window);
}

}  // namespace

TEST_CASE("a Never trigger never fires, window or not", "[compaction_trigger]") {
    CHECK(Resolve(Never()) == 0);
    CHECK(Resolve(Never(), 1'000'000) == 0);
}

TEST_CASE("a fixed trigger is absolute and ignores the window", "[compaction_trigger]") {
    CHECK(Resolve(Fixed(120000)) == 120000);
    CHECK(Resolve(Fixed(120000), 8000) == 120000);
    CHECK(Resolve(Fixed(120000), std::nullopt) == 120000);
}

TEST_CASE("a percent trigger is a fraction of the session's window", "[compaction_trigger]") {
    CHECK(Resolve(Percent(80), 100000) == 80000);
    CHECK(Resolve(Percent(100), 4096) == 4096);
    // Integer arithmetic: no threshold may exceed what the fraction denotes.
    CHECK(Resolve(Percent(50), 99) == 49);
    CHECK(Resolve(Percent(33), 1000) == 330);
}

TEST_CASE("a percent trigger with no declared window never fires", "[compaction_trigger]") {
    // The point of the resolver: a window-relative strategy with nothing to be
    // relative to degrades to "never", not to a guess.
    CHECK(Resolve(Percent(80)) == 0);
}

TEST_CASE("a buffer trigger keeps its distance from the window edge", "[compaction_trigger]") {
    CHECK(Resolve(Buffer(16000), 128000) == 112000);
    CHECK(Resolve(Buffer(1), 4096) == 4095);
}

TEST_CASE("a buffer at least as large as the window leaves nothing to trigger on",
          "[compaction_trigger]") {
    CHECK(Resolve(Buffer(16000), 16000) == 0);
    CHECK(Resolve(Buffer(20000), 16000) == 0);
}

TEST_CASE("a buffer trigger with no declared window never fires", "[compaction_trigger]") {
    CHECK(Resolve(Buffer(16000)) == 0);
}

TEST_CASE("a percent can truncate to zero on a small window", "[compaction_trigger]") {
    // Integer division, so a small enough window turns any percent into "never".
    // Unreachable with a realistic window, but the agent loop's diagnostic used
    // to assert that only a buffer could produce a zero with a window present,
    // and named buffer_tokens for it -- sending the reader to a setting they had
    // not set.
    CHECK(Resolve(Percent(9), 10) == 0);
    CHECK(Resolve(Percent(1), 99) == 0);
    CHECK(Resolve(Percent(1), 100) == 1);
}
