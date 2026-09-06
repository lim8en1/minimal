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
    CHECK(kb.KeyFor("tab.next") == "alt+down");
    CHECK(kb.KeyFor("app.quit") == "ctrl+q");
    CHECK(kb.KeyFor("chat.scroll_bottom") == "pagedown");
    CHECK(kb.KeyFor("nonexistent.action", "x") == "x");
}

TEST_CASE("ApplyDefaults fills an empty table with every core action", "[keybindings]") {
    // The malformed-file fallback (tui_bootstrap's catch arm): Parse("{}") is
    // an EMPTY table, and a broken keybindings.json must degrade to the
    // defaults -- not to "every core action, quit included, unbound".
    auto kb = client::Keybindings::Parse("{}");
    REQUIRE(kb.all().empty());

    kb.ApplyDefaults();

    CHECK(kb.KeyFor("app.quit") == "ctrl+q");
    CHECK(kb.KeyFor("tab.next") == "alt+down");
    CHECK(kb.KeyFor("turn.cancel") == "escape");
}

TEST_CASE("ApplyDefaults never overwrites a binding already present", "[keybindings]") {
    auto kb = client::Keybindings::Parse(R"({"app.quit": "ctrl+9"})");

    kb.ApplyDefaults();

    // The user's own binding wins over the default for the same action -- the
    // same rule Load's default-fill has always applied.
    CHECK(kb.KeyFor("app.quit") == "ctrl+9");
    CHECK(kb.KeyFor("tab.next") == "alt+down");
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

// Regression: bundled/keybindings.json (the file `cmake --install` copies
// into a fresh config dir) fell behind DefaultBindings() by six actions --
// Load's own default-filling (above) means the app still WORKED with the
// missing ones, but a user reading their own keybindings.json to see what's
// bound would never learn those six existed at all. This is the assertion
// Load's tolerance can't provide: the SHIPPED FILE, not just the code
// defaults, must name every core action.
TEST_CASE("bundled/keybindings.json has an entry for every core action",
          "[keybindings]") {
    std::ifstream in(std::filesystem::path(MINIMAL_BUNDLED_DIR) / "keybindings.json");
    REQUIRE(in.is_open());
    const std::string contents((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    const auto bundled = client::Keybindings::Parse(contents);

    // A path that can never exist resolves Load to pure code defaults
    // (its own "fresh config dir" branch) -- the code-side source of truth.
    const auto defaults = client::Keybindings::Load("/nonexistent/minimal-keybindings-sync-check");

    for (const auto& [action, key] : defaults.all()) {
        INFO("action: " << action);
        REQUIRE(bundled.all().count(action) == 1);
        CHECK(bundled.KeyFor(action) == key);
    }
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
