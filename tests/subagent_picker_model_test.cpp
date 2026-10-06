// Pure-logic tier for the /subagents picker's row-building, mirroring
// agent_picker_model_test.cpp -- no FTXUI, no AppState, no network.

#include <catch2/catch_test_macros.hpp>

#include "session_rows.hpp"

using minimal::session_view::BuildSubagentPickerRows;
using minimal::session_view::SubagentPickerRow;

namespace {

minimal::v1::SessionSummary Child(std::string id, std::string name, bool live) {
    minimal::v1::SessionSummary s;
    s.set_session_id(std::move(id));
    s.set_name(std::move(name));
    s.set_is_live(live);
    s.set_message_count(3);
    return s;
}

}  // namespace

TEST_CASE("subagent picker rows: ended children are hidden by default") {
    const std::vector<minimal::v1::SessionSummary> children = {
        Child("live-1", "running child", true),
        Child("done-1", "finished child", false),
    };

    const auto rows = BuildSubagentPickerRows(children, /*include_ended=*/false, "");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].session_id == "live-1");
    CHECK(rows[0].is_live);
}

TEST_CASE("subagent picker rows: ctrl+a's include_ended reveals the ended ones") {
    const std::vector<minimal::v1::SessionSummary> children = {
        Child("done-1", "finished child", false),
        Child("live-1", "running child", true),
        Child("done-2", "errored child", false),
    };

    const auto rows = BuildSubagentPickerRows(children, /*include_ended=*/true, "");
    REQUIRE(rows.size() == 3);
    // Live FIRST, regardless of the delivery order the server sent.
    CHECK(rows[0].session_id == "live-1");
    CHECK(rows[0].is_live);
    // Ended after, in delivery order.
    CHECK(rows[1].session_id == "done-1");
    CHECK_FALSE(rows[1].is_live);
    CHECK(rows[2].session_id == "done-2");
}

TEST_CASE("subagent picker rows: filter matches name or session id") {
    const std::vector<minimal::v1::SessionSummary> children = {
        Child("id-abc", "run the tests", true),
        Child("id-xyz", "write the docs", false),
    };

    SECTION("on the name, case-insensitively") {
        const auto rows = BuildSubagentPickerRows(children, true, "TESTS");
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].session_id == "id-abc");
    }
    SECTION("on the session id") {
        const auto rows = BuildSubagentPickerRows(children, true, "xyz");
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].session_id == "id-xyz");
    }
}

TEST_CASE("subagent picker rows: a nameless child labels itself with the session id") {
    // Subagent rows are created nameless server-side; the raw id (truncated)
    // beats an empty label.
    const std::vector<minimal::v1::SessionSummary> children = {Child("0123456789abcdef", "", true)};

    const auto rows = BuildSubagentPickerRows(children, false, "");
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].label.find("0123456789ab") != std::string::npos);
}

TEST_CASE("subagent picker rows: the label states running vs ended") {
    const std::vector<minimal::v1::SessionSummary> children = {
        Child("live-1", "a live child", true),
        Child("done-1", "an ended child", false),
    };

    const auto rows = BuildSubagentPickerRows(children, true, "");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].label.find("running") != std::string::npos);
    CHECK(rows[1].label.find("ended") != std::string::npos);
}

TEST_CASE("subagent picker rows: an empty child list returns an empty list") {
    CHECK(BuildSubagentPickerRows({}, false, "").empty());
    CHECK(BuildSubagentPickerRows({}, true, "").empty());
}
