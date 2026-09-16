// RadioRowSelected / WrappedOptionEntry: the ask modal's rule that an
// untouched single-pick question shows NO selection. FTXUI's Radiobox clamps
// its selected index from -1 to 0 at first render, so option 0 always reports
// active/focused from frame one -- without the touched gate it would render
// as a pre-picked answer the submission (CollectAnswer, gated on
// radio_touched) would not send.

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/elements.hpp>

#include "minimal/client/view/prompt_modal.hpp"

namespace minimal::client {

TEST_CASE("RadioRowSelected: an untouched question styles no row", "[prompt_modal]") {
    // The clamp makes option 0 active+focused from the first render: this is
    // exactly the row that must NOT wear the choice styling.
    CHECK(RadioRowSelected(/*touched=*/false, /*active=*/true, /*focused=*/true) == false);
    CHECK(RadioRowSelected(false, false, false) == false);
}

TEST_CASE("RadioRowSelected: a touched question styles the current row", "[prompt_modal]") {
    // After the first arrow-down (on_change fired, radio_touched set), the
    // clamped/current row renders as the choice it now genuinely is.
    CHECK(RadioRowSelected(/*touched=*/true, /*active=*/true, /*focused=*/true) == true);
    CHECK(RadioRowSelected(true, true, false) == true);
    CHECK(RadioRowSelected(true, false, false) == true);
}

TEST_CASE("WrappedOptionEntry: the touched flag is the only styling gate",
          "[prompt_modal]") {
    // Marker text is state-driven regardless of touch; the selection
    // decorator rides RadioRowSelected. ftxui elements are opaque, so the
    // observable contract here is that both calls produce an element and the
    // marker text follows s.state -- the styling rule itself is pinned by the
    // RadioRowSelected cases above.
    const ftxui::EntryState untouched{std::string("first"), /*state=*/false,
                                      /*active=*/true, /*focused=*/true, /*index=*/0};
    const ftxui::EntryState touched_on{std::string("first"), /*state=*/true,
                                       /*active=*/true, /*focused=*/true, /*index=*/0};
    const auto selection = SelectionStyle(Theme{});
    const auto muted = Muted(Theme{});
    CHECK_NOTHROW(WrappedOptionEntry(untouched, "◉ ", "○ ", "", selection, muted, false));
    CHECK_NOTHROW(WrappedOptionEntry(touched_on, "◉ ", "○ ", "", selection, muted, true));
}

}  // namespace minimal::client
