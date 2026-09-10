// ModalSlotStale: the pure rebuild-decision for TabMaintenance's SHARED
// permission/confirmation/ask modal slots, extracted specifically so the
// tab-switch regression below is directly testable -- TabMaintenance itself
// needs a real AppState/FTXUI rig (no seam), the same reason
// keybinding_dispatch.cpp pulls its own predicates out as free functions.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/view/tab_ui.hpp"

using minimal::client::ModalSlotStale;

TEST_CASE("ModalSlotStale: a genuinely new request on the SAME tab is stale",
         "[tab_ui]") {
    CHECK(ModalSlotStale("req-1", "req-2", "tab-a", "tab-a"));
}

TEST_CASE("ModalSlotStale: the same request on the same tab is NOT stale",
         "[tab_ui]") {
    CHECK_FALSE(ModalSlotStale("req-1", "req-1", "tab-a", "tab-a"));
}

TEST_CASE("ModalSlotStale: focus moving to another tab is stale even with an "
          "UNCHANGED request id",
         "[tab_ui]") {
    // Regression: tab A gets P1 (slot filled, tracked for A); focus moves to
    // tab B, which raises P2 (slot overwritten, tracked for B); focus
    // returns to A, whose request is STILL P1 -- the request-id check alone
    // reads "unchanged, nothing to rebuild" and the shared slot is left
    // holding B's (possibly already-answered) modal while A's own request
    // is never displayed.
    CHECK(ModalSlotStale("req-1", "req-1", "tab-b", "tab-a"));
}

TEST_CASE("ModalSlotStale: a fresh slot (no tab tracked yet) is stale",
         "[tab_ui]") {
    CHECK(ModalSlotStale("", "req-1", "", "tab-a"));
}
