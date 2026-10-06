// Pure-logic tier for the keymap help overlay's row-building, mirroring
// agent_picker_model_test.cpp -- no FTXUI.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/keymap_help_model.hpp"

using minimal::client::BuildKeymapHelpRows;

namespace {
const std::unordered_map<std::string, std::string> kBindings = {
    {"tab.next", "alt+down"}, {"app.quit", "ctrl+q"}, {"agent.picker", "alt+g"}};
}

TEST_CASE("keymap help rows: grouped by area, key column first, described in words") {
    const auto rows = BuildKeymapHelpRows(kBindings, "");
    REQUIRE(rows.size() == 6);
    CHECK(rows[0].header);
    CHECK(rows[0].label == " tabs");
    CHECK(rows[1].action == "tab.next");
    CHECK(rows[1].label == "alt+down  next tab");
    CHECK(rows[2].label == " open");
    CHECK(rows[3].action == "agent.picker");
    CHECK(rows[3].label == "alt+g     switch agent");   // key column padded to the widest
    CHECK(rows[4].label == " app");
    CHECK(rows[5].action == "app.quit");
    CHECK_FALSE(rows[5].header);
}

TEST_CASE("keymap help rows: within a group, the curated order -- not the alphabet") {
    const auto rows = BuildKeymapHelpRows(
        {{"tab.goto.2", "alt+2"}, {"tab.close", "ctrl+w"}, {"tab.previous", "alt+up"},
         {"tab.next", "alt+down"}, {"group.next", "alt+right"}},
        "");
    REQUIRE(rows.size() == 6);
    CHECK(rows[1].action == "tab.next");
    CHECK(rows[2].action == "tab.previous");
    CHECK(rows[3].action == "tab.goto.2");
    CHECK(rows[4].action == "tab.close");
    CHECK(rows[5].action == "group.next");
}

TEST_CASE("keymap help rows: filter matches the action, the key or the description") {
    SECTION("matches on action name and on description alike") {
        const auto rows = BuildKeymapHelpRows(kBindings, "TAB");
        REQUIRE(rows.size() == 2);   // the "tabs" header + tab.next
        CHECK(rows[1].action == "tab.next");
    }
    SECTION("matches on key") {
        const auto rows = BuildKeymapHelpRows(kBindings, "ALT");
        REQUIRE(rows.size() == 4);   // two groups, one binding each
    }
    SECTION("matches on description only") {
        const auto rows = BuildKeymapHelpRows(kBindings, "switch");
        REQUIRE(rows.size() == 2);
        CHECK(rows[1].action == "agent.picker");
    }
    SECTION("no match: no rows, not even headers") {
        CHECK(BuildKeymapHelpRows(kBindings, "nonexistent").empty());
    }
}

TEST_CASE("keymap help rows: tab.goto.N is described and grouped with the tab keys") {
    const auto rows = BuildKeymapHelpRows({{"tab.goto.3", "alt+3"}}, "");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].label == " tabs");
    CHECK(rows[1].label == "alt+3  go to tab 3");
}

TEST_CASE("keymap help rows: an unknown (plugin) action is listed under plugins by its id") {
    const auto rows = BuildKeymapHelpRows({{"checklist.toggle", "ctrl+k"}}, "");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].label == " plugins");
    CHECK(rows[1].label == "ctrl+k  checklist.toggle");
}

TEST_CASE("core describes its key actions for plugins: words, group, and the listing order",
          "[keymap_help]") {
    using minimal::client::DescribeKeyAction;
    const auto next = DescribeKeyAction("tab.next");
    CHECK(next.description == "next tab");
    CHECK(next.group == "tabs");
    // Groups in display order, and the curated order within one -- not the alphabet.
    CHECK(next.order < DescribeKeyAction("tab.previous").order);
    CHECK(DescribeKeyAction("tab.goto.9").order < DescribeKeyAction("tab.close").order);
    CHECK(DescribeKeyAction("tab.close").order < DescribeKeyAction("chat.scroll_up").order);
    CHECK(DescribeKeyAction("chat.scroll_up").group == "chat");
    // Unknown to core (a plugin view's toggle): grouped last, named by its id.
    const auto view = DescribeKeyAction("config");
    CHECK(view.group == "views");
    CHECK(view.description == "config");
    CHECK(DescribeKeyAction("app.quit").order < view.order);
}
