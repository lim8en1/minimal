#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "minimal/llm/token_accounting.hpp"

using namespace minimal::llm;

TEST_CASE("context occupancy uses only the last turn, never a running sum", "[token_accounting]") {
    TokenAccumulator acc;
    acc.RecordTurn(Usage{100, 20, 0, 0});
    acc.RecordTurn(Usage{500, 30, 0, 0});  // a later turn's input already contains prior history
    acc.RecordTurn(Usage{900, 40, 0, 0});

    CHECK(acc.last_occupancy() == 940);  // 900 + 40, NOT the sum across all three turns
}

TEST_CASE("cumulative totals are additive across turns", "[token_accounting]") {
    TokenAccumulator acc;
    acc.RecordTurn(Usage{100, 20, 0, 0});
    acc.RecordTurn(Usage{500, 30, 0, 0});

    auto totals = acc.cumulative();
    CHECK(totals.total_output_tokens == 50);
    CHECK(totals.total_input_tokens == 600);
}

TEST_CASE("a zero-usage turn contributes 0, not a crash", "[token_accounting]") {
    TokenAccumulator acc;
    acc.RecordTurn(Usage{100, 20, 0, 0});
    acc.RecordTurn(Usage{0, 0, 0, 0});

    CHECK(acc.last_occupancy() == 0);
    CHECK(acc.cumulative().total_output_tokens == 20);
}

TEST_CASE("context_occupancy is a pure function of one Usage", "[token_accounting]") {
    CHECK(context_occupancy(Usage{10, 5, 0, 0}) == 15);
}

TEST_CASE("ReportedUsage: all-zero is the proxy gap, any field is a report", "[token_accounting]") {
    CHECK_FALSE(ReportedUsage(Usage{}));
    CHECK_FALSE(ReportedUsage(Usage{0, 0, 0, 0}));
    CHECK(ReportedUsage(Usage{1, 0, 0, 0}));
    CHECK(ReportedUsage(Usage{0, 1, 0, 0}));
    CHECK(ReportedUsage(Usage{0, 0, 1, 0}));
    CHECK(ReportedUsage(Usage{0, 0, 0, 1}));
}

TEST_CASE("SeedOccupancy overwrites the last-turn occupancy without touching totals",
          "[token_accounting]") {
    // The zero-usage fallback's contract: occupancy describes the context
    // (estimated), cumulative totals stay honestly zero for that turn.
    TokenAccumulator acc;
    acc.RecordTurn(Usage{100, 20, 0, 0});
    acc.SeedOccupancy(5000);

    CHECK(acc.last_occupancy() == 5000);
    CHECK(acc.cumulative().total_output_tokens == 20);
    CHECK(acc.cumulative().total_input_tokens == 100);

    // A subsequent real report replaces the estimate wholesale.
    acc.RecordTurn(Usage{900, 40, 0, 0});
    CHECK(acc.last_occupancy() == 940);
    CHECK(acc.cumulative().total_input_tokens == 1000);
}

TEST_CASE("context occupancy counts cache tokens, which are context too", "[token_accounting]") {
    // input_tokens is the UNCACHED portion (Anthropic's convention, which
    // provider-openai's ParseUsage now normalizes to). The context served from
    // / written to the prompt cache must be added back or the status bar and
    // the compaction trigger under-count cached sessions.
    CHECK(context_occupancy(Usage{10, 5, 80, 5}) == 100);

    TokenAccumulator acc;
    acc.RecordTurn(Usage{900, 40, 800, 0});
    CHECK(acc.last_occupancy() == 1740);
    CHECK(acc.last_input_tokens() == 900);  // the raw field stays untouched
}

TEST_CASE("SeedOccupancy sets the trigger input without touching cumulative totals",
          "[token_accounting]") {
    // Resume seeds an ESTIMATE of the loaded context so the compaction trigger
    // can fire before the first provider call. The cumulative totals are
    // genuinely additive quantities -- a synthetic seed must not pollute them.
    TokenAccumulator acc;
    acc.SeedOccupancy(4000);

    CHECK(acc.last_occupancy() == 4000);
    CHECK(acc.last_input_tokens() == 4000);
    CHECK(acc.cumulative().total_input_tokens == 0);
    CHECK(acc.cumulative().total_output_tokens == 0);

    // The first real turn replaces the estimate, and only real usage accumulates.
    acc.RecordTurn(Usage{5000, 100, 0, 0});
    CHECK(acc.last_occupancy() == 5100);
    CHECK(acc.cumulative().total_input_tokens == 5000);
    CHECK(acc.cumulative().total_output_tokens == 100);
}

TEST_CASE("SeedOccupancy saturates instead of truncating", "[token_accounting][invariant]") {
    // The comment long claimed a clamp while the bare static_cast<int> WRAPPED:
    // an estimate past INT_MAX re-entered as a small (or negative) number and
    // mis-seeded the compaction trigger. No real context window approaches
    // INT_MAX tokens, so saturation loses nothing -- and never lies.
    TokenAccumulator acc;
    const std::int64_t huge = static_cast<std::int64_t>(std::numeric_limits<int>::max()) + 1234;
    acc.SeedOccupancy(huge);

    CHECK(acc.last_input_tokens() == std::numeric_limits<int>::max());
    CHECK(acc.last_occupancy() == std::numeric_limits<int>::max());
}

TEST_CASE("negative usage cannot suppress the compaction trigger", "[token_accounting][invariant]") {
    // The trigger compares occupancy < threshold, so a provider reporting a
    // negative value (sentinel, misparse, hostile gateway) would silently
    // disable compaction for the whole session. Fields clamp at zero in the
    // occupancy sum; the raw fields stay untouched for the status bar.
    const Usage negative{-500, -10, -1000, -5};
    CHECK(context_occupancy(negative) == 0);

    TokenAccumulator acc;
    acc.RecordTurn(negative);
    CHECK(acc.last_occupancy() == 0);
    CHECK(acc.last_input_tokens() == -500);  // raw field preserved
}
