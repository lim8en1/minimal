// ResolveSelectableRow: the filterable menu shell's header-skipping rule.
// The model picker's provider headers are real rows (so FTXUI's Menu can own
// the list), but the cursor must never rest on one -- every path that can
// land the selection on a header (arrow moves Menu itself performed, filter
// rebuilds that shrink the list, the first render of a list whose row 0 is a
// header) resolves through this one pure function.

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

#include "ftxui_harness.hpp"
#include "minimal/client/render/theme.hpp"
#include "filterable_menu.hpp"

namespace minimal::client::ui_kit {
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

namespace {

struct MenuFixture {
    Theme theme = Theme::Parse(R"({"name":"t","colors":{}})", ColorDepth::Truecolor);
    std::vector<std::string> selected;
    int closes = 0;

    ftxui::Component Make(std::vector<MenuRow> rows, MenuStatus status = {}) {
        FilterableMenuSpec spec{.window_title = "t", .empty_message = " (nothing)"};
        spec.status = std::move(status);
        return MakeFilterableMenuModal(
            [rows](const std::string& filter) {
                std::vector<MenuRow> out;
                for (const auto& row : rows) {
                    if (row.label.find(filter) != std::string::npos) out.push_back(row);
                }
                return out;
            },
            spec,
            {.on_select = [this](const MenuRow& row) { selected.push_back(row.id); },
             .on_close = [this] { ++closes; },
             .on_extra_key = nullptr},
            theme);
    }
};

}  // namespace

TEST_CASE("filterable menu: Esc clears a typed filter before it closes",
          "[filterable_menu]") {
    MenuFixture fx;
    auto menu = fx.Make({Item("alpha"), Item("beta")});
    minimal_test::SendEvent(menu, ftxui::Event::Character("b"));
    CHECK(minimal_test::RenderFrame(menu).find("alpha") == std::string::npos);
    minimal_test::SendEvent(menu, ftxui::Event::Escape);
    CHECK(fx.closes == 0);
    CHECK(minimal_test::RenderFrame(menu).find("alpha") != std::string::npos);
    minimal_test::SendEvent(menu, ftxui::Event::Escape);
    CHECK(fx.closes == 1);
}

TEST_CASE("filterable menu: ctrl+u clears the filter; control bytes are not filter text",
          "[filterable_menu]") {
    MenuFixture fx;
    auto menu = fx.Make({Item("alpha"), Item("beta")});
    minimal_test::SendEvent(menu, ftxui::Event::Character("zz"));
    CHECK(minimal_test::RenderFrame(menu).find("(nothing)") != std::string::npos);
    minimal_test::SendEvent(menu, ftxui::Event::Character(std::string(1, '\x15')));
    CHECK(minimal_test::RenderFrame(menu).find("alpha") != std::string::npos);
    minimal_test::SendEvent(menu, ftxui::Event::Character(std::string(1, '\x02')));   // ctrl+b
    CHECK(minimal_test::RenderFrame(menu).find("alpha") != std::string::npos);
}

TEST_CASE("filterable menu: MenuMemory carries filter and cursor into a rebuilt component",
          "[filterable_menu][regression]") {
    // A picker rebuilds its component when fetched data lands; type-ahead
    // and the cursor used to reset on every such rebuild.
    MenuFixture fx;
    auto memory = std::make_shared<MenuMemory>();
    auto first = fx.Make({Item("apple"), Item("apricot"), Item("banana")},
                         MenuStatus{.loading = true, .error = {}, .memory = memory});
    minimal_test::SendEvent(first, ftxui::Event::Character("ap"));
    minimal_test::SendEvent(first, ftxui::Event::ArrowDown);
    (void)minimal_test::RenderFrame(first);
    CHECK(memory->filter == "ap");
    CHECK(memory->selected_id == "apricot");

    auto rebuilt = fx.Make({Item("apple"), Item("apricot"), Item("banana"), Item("apex")},
                           MenuStatus{.loading = false, .error = {}, .memory = memory});
    const auto frame = minimal_test::RenderFrame(rebuilt);
    CHECK(frame.find("banana") == std::string::npos);   // the filter survived
    minimal_test::SendEvent(rebuilt, ftxui::Event::Return);
    REQUIRE(fx.selected.size() == 1);
    CHECK(fx.selected.front() == "apricot");            // so did the cursor
}

TEST_CASE("filterable menu: an empty list says loading, or why the fetch failed",
          "[filterable_menu]") {
    MenuFixture fx;
    auto loading = fx.Make({}, MenuStatus{.loading = true, .error = {}, .memory = nullptr});
    const auto loading_frame = minimal_test::RenderFrame(loading);
    CHECK(loading_frame.find("loading") != std::string::npos);
    CHECK(loading_frame.find("(nothing)") == std::string::npos);

    auto failed =
        fx.Make({}, MenuStatus{.loading = false, .error = "deadline exceeded", .memory = nullptr});
    CHECK(minimal_test::RenderFrame(failed).find("deadline exceeded") != std::string::npos);
}

}  // namespace minimal::client::ui_kit
