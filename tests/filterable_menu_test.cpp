// ResolveSelectableRow: the filterable menu shell's header-skipping rule.
// The model picker's provider headers are real rows (so FTXUI's Menu can own
// the list), but the cursor must never rest on one -- every path that can
// land the selection on a header (arrow moves Menu itself performed, filter
// rebuilds that shrink the list, the first render of a list whose row 0 is a
// header) resolves through this one pure function.

#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {
namespace {

MenuRow Header(const char* label) {
    return MenuRow{label, "", /*header=*/true};
}
MenuRow Item(const char* label) {
    return MenuRow{label, label, /*header=*/false};
}
// Readable row lists: Rows(Item("a"), Header("h"), Item("b")).
template <typename... Rest>
std::vector<MenuRow> Rows(MenuRow first, Rest... rest) {
    return {first, rest...};
}

}  // namespace

TEST_CASE("ResolveSelectableRow: a selectable `from` stays", "[filterable_menu]") {
    const auto rows = Rows(Item("a"), Header("h"), Item("b"));
    CHECK(ResolveSelectableRow(rows, 0, 1) == 0);
    CHECK(ResolveSelectableRow(rows, 0, -1) == 0);
    CHECK(ResolveSelectableRow(rows, 2, 1) == 2);
    CHECK(ResolveSelectableRow(rows, 2, -1) == 2);
}

TEST_CASE("ResolveSelectableRow: a header lands on the nearest selectable row "
          "in the travel direction",
          "[filterable_menu]") {
    const auto rows = Rows(Item("a"), Header("h"), Item("b"), Item("c"));
    // Moved DOWN onto the header: the next row below.
    CHECK(ResolveSelectableRow(rows, 1, 1) == 2);
    // Moved UP onto the header: the next row above.
    CHECK(ResolveSelectableRow(rows, 1, -1) == 0);
}

TEST_CASE("ResolveSelectableRow: an exhausted direction falls back to the "
          "opposite side",
          "[filterable_menu]") {
    // Header at the very end (a filter can shrink a group to nothing below):
    // a down move must land BACK UP, not wrap or stick.
    const auto ends_in_header = Rows(Item("a"), Header("h"));
    CHECK(ResolveSelectableRow(ends_in_header, 1, 1) == 0);
    // And the mirror: a header at the very top, moving up.
    const auto starts_in_header = Rows(Header("h"), Item("a"));
    CHECK(ResolveSelectableRow(starts_in_header, 0, -1) == 1);
}

TEST_CASE("ResolveSelectableRow: out-of-bounds `from` scans in the direction, "
          "then falls back",
          "[filterable_menu]") {
    // Down past the end (Menu clamps, but a rebuild can shrink the list
    // first): land on the last selectable row, not the first.
    const auto rows = Rows(Item("a"), Header("h"), Item("b"));
    CHECK(ResolveSelectableRow(rows, 3, 1) == 2);
    CHECK(ResolveSelectableRow(rows, 3, -1) == 2);
    // Up past the start: land on the first selectable row.
    CHECK(ResolveSelectableRow(rows, -1, -1) == 0);
    CHECK(ResolveSelectableRow(rows, -1, 1) == 0);
}

TEST_CASE("ResolveSelectableRow: the model picker's real shape -- a leading "
          "header selects the first model",
          "[filterable_menu]") {
    // Row 0 is a provider header on every unfiltered open of /model: without
    // resolution the cursor's first resting place is a row that renders no
    // selection at all.
    const auto rows = Rows(Header("anthropic"), Item("claude"),
                                       Header("openrouter"), Item("qwen"));
    CHECK(ResolveSelectableRow(rows, 0, 1) == 1);
    CHECK(ResolveSelectableRow(rows, 0, -1) == 1);
}

TEST_CASE("ResolveSelectableRow: a list with no selectable row yields -1", "[filterable_menu]") {
    // Only headers: the empty_message path, not a crash or a stuck cursor.
    const auto headers_only = Rows(Header("h"), Header("h2"));
    CHECK(ResolveSelectableRow(headers_only, 0, 1) == -1);
    CHECK(ResolveSelectableRow(headers_only, 1, -1) == -1);
    // Empty list entirely.
    CHECK(ResolveSelectableRow({}, 0, 1) == -1);
}

}  // namespace minimal::client
