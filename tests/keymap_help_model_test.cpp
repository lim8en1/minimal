// Pure-logic tier for the keymap help overlay's row-building, mirroring
// agent_picker_model_test.cpp -- no FTXUI.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/keymap_help_model.hpp"

using minimal::client::BuildKeymapHelpRows;

TEST_CASE("keymap help rows: no filter returns every binding, sorted by action") {
    const std::unordered_map<std::string, std::string> bindings = {
        {"tab.next", "alt+down"}, {"app.quit", "ctrl+q"}, {"agent.picker", "alt+g"}};

    const auto rows = BuildKeymapHelpRows(bindings, "");
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].action == "agent.picker");
    CHECK(rows[1].action == "app.quit");
    CHECK(rows[2].action == "tab.next");
    CHECK(rows[0].label == "agent.picker  --  alt+g");
}

TEST_CASE("keymap help rows: filter matches the action name or the key, case-insensitively") {
    const std::unordered_map<std::string, std::string> bindings = {
        {"tab.next", "alt+down"}, {"app.quit", "ctrl+q"}, {"agent.picker", "alt+g"}};

    SECTION("matches on action name") {
        const auto rows = BuildKeymapHelpRows(bindings, "TAB");
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].action == "tab.next");
    }
    SECTION("matches on key") {
        const auto rows = BuildKeymapHelpRows(bindings, "ALT");
        REQUIRE(rows.size() == 2);   // tab.next (alt+down), agent.picker (alt+g)
    }
    SECTION("no match") {
        CHECK(BuildKeymapHelpRows(bindings, "nonexistent").empty());
    }
}
