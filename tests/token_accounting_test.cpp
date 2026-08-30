#include <catch2/catch_test_macros.hpp>

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
