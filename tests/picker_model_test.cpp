// Pure-logic tier for the session picker's row-building (T8): index and
// filter math over a value type app.cpp already fetches, extracted the same
// way keybinding_dispatch_test.cpp covers RunTui's global-keybinding math --
// no FTXUI, no AppState, no network.

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/picker_model.hpp"

using minimal::client::BuildPickerRows;
using minimal::client::PickerRow;
using minimal::client::PickerSort;

namespace {

minimal::v1::SessionSummary Session(std::string id, std::string name, std::string project_root,
                                    int64_t updated_at_unix_ms, int32_t message_count = 1) {
    minimal::v1::SessionSummary s;
    s.set_session_id(std::move(id));
    s.set_name(std::move(name));
    s.set_project_root(std::move(project_root));
    s.set_updated_at_unix_ms(updated_at_unix_ms);
    s.set_message_count(message_count);
    return s;
}

}  // namespace

TEST_CASE("picker rows: the synthetic new-session row is always first") {
    const std::vector<minimal::v1::SessionSummary> sessions = {
        Session("s1", "alpha", "/proj", 100)};

    const auto rows = BuildPickerRows(sessions, "", PickerSort::kRecent);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].session_id.empty());
    CHECK(rows[0].label == "+ new session");
    CHECK(rows[1].session_id == "s1");
}

TEST_CASE("picker rows: new-session row survives a filter that matches nothing") {
    const std::vector<minimal::v1::SessionSummary> sessions = {
        Session("s1", "alpha", "/proj", 100)};

    const auto rows = BuildPickerRows(sessions, "zzz-does-not-match", PickerSort::kRecent);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].session_id.empty());
}

TEST_CASE("picker rows: filter matches name, id, or project_root case-insensitively") {
    const std::vector<minimal::v1::SessionSummary> sessions = {
        Session("s1", "Fix The Bug", "/home/user/proj-a", 100),
        Session("s2", "unrelated", "/home/user/proj-b", 90),
        Session("s3", "also unrelated", "/home/user/other", 80),
    };

    SECTION("matches on name") {
        const auto rows = BuildPickerRows(sessions, "the bug", PickerSort::kRecent);
        REQUIRE(rows.size() == 2);   // new-session row + s1
        CHECK(rows[1].session_id == "s1");
    }
    SECTION("matches on session id") {
        const auto rows = BuildPickerRows(sessions, "S2", PickerSort::kRecent);
        REQUIRE(rows.size() == 2);
        CHECK(rows[1].session_id == "s2");
    }
    SECTION("matches on project_root") {
        const auto rows = BuildPickerRows(sessions, "proj-", PickerSort::kRecent);
        REQUIRE(rows.size() == 3);   // new-session row + s1 + s2, not s3
        CHECK(rows[1].session_id == "s1");
        CHECK(rows[2].session_id == "s2");
    }
}

TEST_CASE("picker rows: kRecent sorts newest updated_at first") {
    const std::vector<minimal::v1::SessionSummary> sessions = {
        Session("old", "old", "/p", 10),
        Session("newest", "newest", "/p", 30),
        Session("mid", "mid", "/p", 20),
    };

    const auto rows = BuildPickerRows(sessions, "", PickerSort::kRecent);
    REQUIRE(rows.size() == 4);
    CHECK(rows[1].session_id == "newest");
    CHECK(rows[2].session_id == "mid");
    CHECK(rows[3].session_id == "old");
}

TEST_CASE("picker rows: kName sorts case-insensitively, ties broken by session id") {
    const std::vector<minimal::v1::SessionSummary> sessions = {
        Session("z-tie", "Same", "/p", 10),
        Session("a-tie", "same", "/p", 20),
        Session("mid", "Bravo", "/p", 15),
    };

    const auto rows = BuildPickerRows(sessions, "", PickerSort::kName);
    REQUIRE(rows.size() == 4);
    CHECK(rows[1].session_id == "mid");      // "Bravo" < "same"
    CHECK(rows[2].session_id == "a-tie");    // tie on name, "a-tie" < "z-tie"
    CHECK(rows[3].session_id == "z-tie");
}

TEST_CASE("picker rows: sorting is a display transform -- it does not mutate caller order, "
         "and repeated calls over the same input are stable") {
    const std::vector<minimal::v1::SessionSummary> sessions = {
        Session("a", "a", "/p", 10), Session("b", "b", "/p", 20)};

    const auto first = BuildPickerRows(sessions, "", PickerSort::kName);
    // The caller's vector (what "load more" derives its cursor from) is
    // unchanged regardless of what sort was requested.
    CHECK(sessions[0].session_id() == "a");
    CHECK(sessions[1].session_id() == "b");

    const auto second = BuildPickerRows(sessions, "", PickerSort::kName);
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].session_id == second[i].session_id);
    }
}

TEST_CASE("picker rows: an empty session list still offers the new-session row") {
    const auto rows = BuildPickerRows({}, "", PickerSort::kRecent);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].session_id.empty());
}
