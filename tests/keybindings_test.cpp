#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>

#include "minimal/client/keybindings.hpp"
#include "minimal/client/status_format.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;

// --- keybindings ------------------------------------------------------------

TEST_CASE("Keybindings::Parse reads action -> key pairs", "[keybindings]") {
    const auto kb = client::Keybindings::Parse(R"({"tab.close": "ctrl+w", "app.quit": "ctrl+q"})");

    CHECK(kb.KeyFor("tab.close") == "ctrl+w");
    CHECK(kb.KeyFor("app.quit") == "ctrl+q");
}

TEST_CASE("unknown action names are kept, not rejected", "[keybindings]") {
    // Client plugins (E5) register views addressable from this file by
    // runtime-chosen ids. A file written for a newer build must load here, and
    // a file this build writes must survive an older build. Proven
    // load-bearing: throwing on unknown names fails this case.
    const auto kb = client::Keybindings::Parse(R"({"some_future_plugin.view": "ctrl+7"})");

    REQUIRE(kb.all().count("some_future_plugin.view") == 1);
    CHECK(kb.KeyFor("some_future_plugin.view") == "ctrl+7");
}

TEST_CASE("a non-string value is a malformed file, named", "[keybindings]") {
    CHECK_THROWS_WITH(client::Keybindings::Parse(R"({"tab.close": 7})"),
                      Catch::Matchers::ContainsSubstring("tab.close"));
}

TEST_CASE("malformed JSON is rejected with a reason", "[keybindings]") {
    CHECK_THROWS(client::Keybindings::Parse("{oops"));
    CHECK_THROWS(client::Keybindings::Parse("[]"));
}

TEST_CASE("Load on a fresh config dir yields the defaults", "[keybindings]") {
    TempDir dir;

    const auto kb = client::Keybindings::Load(dir.path());

    // The core actions all carry defaults; a first run is usable, not a blank
    // slate the user has to author before anything works.
    CHECK(kb.KeyFor("tab.next") == "ctrl+tab");
    CHECK(kb.KeyFor("app.quit") == "ctrl+q");
    CHECK(kb.KeyFor("nonexistent.action", "x") == "x");
}

TEST_CASE("Load reads the user's file and fills only what it omits", "[keybindings]") {
    TempDir dir;
    { std::ofstream out(dir.path() / "keybindings.json"); out << R"({"tab.next": "ctrl+l"})"; }

    const auto kb = client::Keybindings::Load(dir.path());

    // The user's binding wins for the action they named...
    CHECK(kb.KeyFor("tab.next") == "ctrl+l");
    // ...and the defaults still stand for everything they did not. An upgrade
    // that adds a core action must work without rewriting anyone's file.
    CHECK(kb.KeyFor("app.quit") == "ctrl+q");
}

// --- status format ----------------------------------------------------------

TEST_CASE("status format splits literals from segments", "[status_format]") {
    const auto parts = client::ParseStatusFormat("{version} | {tokens}");

    REQUIRE(parts.size() == 3);
    CHECK(parts[0].is_segment);
    CHECK(parts[0].text == "version");
    CHECK_FALSE(parts[1].is_segment);
    CHECK(parts[1].text == " | ");
    CHECK(parts[2].is_segment);
    CHECK(parts[2].text == "tokens");
}

TEST_CASE("adjacent segments and empty literals survive", "[status_format]") {
    const auto parts = client::ParseStatusFormat("{version}{tokens}");

    REQUIRE(parts.size() == 2);
    CHECK(parts[0].text == "version");
    CHECK(parts[1].text == "tokens");
}

TEST_CASE("unbalanced braces are literal text", "[status_format]") {
    const auto parts = client::ParseStatusFormat("{version | {tokens}");

    // The trailing "{version | " is decoration the user typed; inventing an
    // error channel for it would be worse than rendering it.
    REQUIRE(parts.size() == 2);
    CHECK_FALSE(parts[0].is_segment);
    CHECK(parts[0].text == "{version | ");
    CHECK(parts[1].text == "tokens");
}
