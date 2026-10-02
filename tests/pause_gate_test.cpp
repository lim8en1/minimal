#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/pause_gate.hpp"

using minimal::agent::PauseGate;

TEST_CASE("PauseGate transitions preserve the other bit", "[session][pause_gate]") {
    PauseGate gate;
    CHECK_FALSE(gate.paused());
    CHECK_FALSE(gate.continuation_pending());

    SECTION("SetPaused reports change and keeps continuation") {
        gate.MarkContinuationPending();
        CHECK(gate.SetPaused(true));
        CHECK(gate.paused());
        CHECK(gate.continuation_pending());
        CHECK_FALSE(gate.SetPaused(true));
        CHECK(gate.SetPaused(false));
        CHECK_FALSE(gate.paused());
        CHECK(gate.continuation_pending());  // the not-paused-but-owed state
    }
    SECTION("ClearPaused keeps continuation; ClearContinuation keeps paused") {
        gate.SetPaused(true);
        gate.MarkContinuationPending();
        gate.ClearPaused();
        CHECK_FALSE(gate.paused());
        CHECK(gate.continuation_pending());
        gate.SetPaused(true);
        gate.ClearContinuation();
        CHECK(gate.paused());
        CHECK_FALSE(gate.continuation_pending());
    }
    SECTION("clearing an unset bit is a no-op") {
        gate.ClearPaused();
        gate.ClearContinuation();
        CHECK_FALSE(gate.paused());
        CHECK_FALSE(gate.continuation_pending());
    }
    SECTION("MarkContinuationPending keeps paused") {
        gate.SetPaused(true);
        gate.MarkContinuationPending();
        CHECK(gate.paused());
        CHECK(gate.continuation_pending());
    }
}
