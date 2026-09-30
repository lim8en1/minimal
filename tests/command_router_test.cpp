// CommandRouter: the client-side /-command table (listing + behavior in one).

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "minimal/client/command_router.hpp"

using minimal::client::ClientCommand;
using minimal::client::CommandInvocationText;
using minimal::client::CommandRouter;

TEST_CASE("command router: listed commands become catalog entries, intercepts do not",
          "[command_router]") {
    CommandRouter router;
    router.Add({"new", "start a new session", false, false, [](const auto&) { return true; }});
    router.Add({"agent", "", false, false, [](const auto&) { return false; }});
    const auto entries = router.CatalogEntries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].name == "new");
    CHECK(entries[0].summary == "start a new session");
    CHECK_FALSE(entries[0].requires_session);
}

TEST_CASE("command router: dispatch runs the named command with its arguments",
          "[command_router]") {
    CommandRouter router;
    std::vector<std::string> seen;
    router.Add({"rewind", "undo", false, true, [&](const CommandInvocationText& parsed) {
                    seen = parsed.positional;
                    return true;
                }});
    CHECK(router.Dispatch("/rewind 2"));
    CHECK(seen == std::vector<std::string>{"2"});
    CHECK_FALSE(router.Dispatch("/cost"));       // not the client's
    CHECK_FALSE(router.Dispatch("plain text"));  // not a command
}

TEST_CASE("command router: an intercept can pass a form on to the server",
          "[command_router]") {
    CommandRouter router;
    int opened = 0;
    router.Add({"help", "", true, false, [&](const CommandInvocationText& parsed) {
                    if (!parsed.positional.empty()) return false;   // /help <name>: server's
                    ++opened;
                    return true;
                }});
    CHECK(router.Dispatch("/help"));
    CHECK_FALSE(router.Dispatch("/help cost"));
    CHECK(opened == 1);
}
