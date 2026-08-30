// The client's command line.
//
// Untested until now, which is how three defects lived in twenty lines of
// argv loop. All three were silent, and they compounded:
//
//   minimal -p --server unix:///tmp/x.sock "read the file"
//
// sent the model the prompt "--server", dropped the target, dropped the real
// prompt, and then reported that it could not reach the DEFAULT socket. Each
// symptom pointed away from the cause.

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "minimal/client/cli_args.hpp"

using namespace minimal::client;

namespace {

ParsedArgs Parse(std::vector<const char*> args) {
    args.insert(args.begin(), "minimal");
    return ParseArgs(static_cast<int>(args.size()), args.data());
}

}  // namespace

TEST_CASE("the -p flag does not swallow the next flag as its prompt", "[client_args][invariant]") {
    // THE BUG, in the order it was actually typed. Nothing here is exotic --
    // putting the flag that says where the server is before the prompt is the
    // obvious way to write it.
    const auto parsed = Parse({"-p", "--server", "unix:///tmp/x.sock", "read the file"});

    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("--server") != std::string::npos);
    // The message has to carry the fix, not just the complaint: the reason this
    // took a while to spot is that every symptom pointed somewhere else.
    CHECK(parsed.error.find("-p=--server") != std::string::npos);
}

TEST_CASE("the same command in the other order works", "[client_args]") {
    const auto parsed = Parse({"--server", "unix:///tmp/x.sock", "-p", "read the file"});

    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    CHECK(parsed.headless.prompt == "read the file");
    CHECK(parsed.headless.server == "unix:///tmp/x.sock");
}

TEST_CASE("an unrecognised argument is an error, not a shrug", "[client_args][invariant]") {
    // Silently dropping these is what let the case above stay quiet: the
    // swallowed target and the real prompt both landed here and vanished.
    SECTION("a stray value") {
        const auto parsed = Parse({"-p", "hello", "extra-thing"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("extra-thing") != std::string::npos);
    }

    SECTION("a misspelled flag") {
        const auto parsed = Parse({"-p", "hello", "--serverr", "unix:///tmp/x.sock"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("--serverr") != std::string::npos);
    }
}

TEST_CASE("a dash-leading value is still expressible", "[client_args]") {
    // The cost of rejecting flag-shaped values positionally. If this did not
    // work the rule would be a bug of its own -- "-v" is a plausible prompt.
    const auto parsed = Parse({"--print=--this is the prompt"});

    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    CHECK(parsed.headless.prompt == "--this is the prompt");
}

TEST_CASE("the flag=value form works for every value-taking flag", "[client_args]") {
    const auto parsed =
        Parse({"--print=hi", "--server=unix:///tmp/x.sock", "--agent=plan",
               "--output-format=jsonl"});

    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    CHECK(parsed.headless.prompt == "hi");
    CHECK(parsed.headless.server == "unix:///tmp/x.sock");
    CHECK(parsed.headless.agent_preset == "plan");
    CHECK(parsed.headless.jsonl);
}

TEST_CASE("a flag at the end of the line reports the missing value", "[client_args]") {
    const auto parsed = Parse({"-p"});
    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("needs a value") != std::string::npos);
}

TEST_CASE("the server flag accepts what the listen flag accepts", "[client_args][invariant]") {
    // The asymmetry that made this a trap: `minimal-server --listen` takes a
    // bare filesystem path, so the obvious pairing --server /tmp/x.sock failed
    // with a gRPC resolver error naming neither flag. Both halves of the pair
    // now accept the same thing.
    SECTION("an absolute path becomes a unix target") {
        CHECK(ResolveServerTarget("/tmp/x.sock") == "unix:///tmp/x.sock");
    }

    SECTION("an explicit target is passed through untouched") {
        CHECK(ResolveServerTarget("unix:///tmp/x.sock") == "unix:///tmp/x.sock");
        CHECK(ResolveServerTarget("dns:///example.internal:9000") ==
              "dns:///example.internal:9000");
    }

    SECTION("host:port is not a path") {
        CHECK(ResolveServerTarget("localhost:9000") == "localhost:9000");
        CHECK(ResolveServerTarget("127.0.0.1:9000") == "127.0.0.1:9000");
    }

    SECTION("a relative socket path is made absolute here, not by gRPC") {
        // gRPC would resolve it against its own notion of the cwd rather than
        // the user's, which is a difference nobody wants to discover at runtime.
        const auto target = ResolveServerTarget("./run/x.sock");
        CHECK(target.rfind("unix:///", 0) == 0);
        CHECK(target.find("/run/x.sock") != std::string::npos);
        CHECK(target.find("./") == std::string::npos);
    }
}

TEST_CASE("no -p means the TUI, and flags alone do not imply headless", "[client_args]") {
    CHECK(Parse({}).action == ParsedArgs::Action::Tui);
    CHECK(Parse({"--server", "unix:///tmp/x.sock"}).action == ParsedArgs::Action::Tui);
}

TEST_CASE("help and version win over everything else", "[client_args]") {
    CHECK(Parse({"--help"}).action == ParsedArgs::Action::Usage);
    CHECK(Parse({"-h"}).action == ParsedArgs::Action::Usage);
    CHECK(Parse({"--version"}).action == ParsedArgs::Action::Version);
    // Even alongside a broken invocation: asking what the flags are is exactly
    // what someone does after getting the arguments wrong.
    CHECK(Parse({"-p", "hi", "--version"}).action == ParsedArgs::Action::Version);
}

TEST_CASE("an unknown --output-format is refused", "[client_args]") {
    const auto parsed = Parse({"-p", "hi", "--output-format", "yaml"});
    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("yaml") != std::string::npos);
}

TEST_CASE("the usage text names both halves of the socket pairing", "[client_args]") {
    // The fix for a trap is only delivered if the person who hits it reads it,
    // and usage is what they see.
    const auto usage = UsageText();
    CHECK(usage.find("--listen") != std::string::npos);
    CHECK(usage.find("--flag=value") != std::string::npos);
}

// --- Resume --------------------------------------------------------------------

TEST_CASE("the resume flag carries a session id to a headless turn", "[client_args][resume]") {
    const auto parsed =
        Parse({"-p", "what number did I give you?", "--resume", "01ARZ3NDEKTSV4RRFFQ69G5FAV"});

    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    CHECK(parsed.headless.prompt == "what number did I give you?");
    CHECK(parsed.headless.resume_session_id == "01ARZ3NDEKTSV4RRFFQ69G5FAV");
}

TEST_CASE("the resume flag follows the same value rules as every other flag",
          "[client_args][resume]") {
    SECTION("a flag-shaped value is refused") {
        const auto parsed = Parse({"-p", "hi", "--resume", "--agent"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("--resume") != std::string::npos);
    }
    SECTION("a missing value at the end of the line") {
        const auto parsed = Parse({"-p", "hi", "--resume"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
    }
    SECTION("the flag=value form works") {
        const auto parsed = Parse({"-p", "hi", "--resume=01ARZ3NDEKTSV4RRFFQ69G5FAV"});
        REQUIRE(parsed.action == ParsedArgs::Action::Headless);
        CHECK(parsed.headless.resume_session_id == "01ARZ3NDEKTSV4RRFFQ69G5FAV");
    }
    SECTION("an empty value is refused") {
        const auto parsed = Parse({"-p", "hi", "--resume="});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
    }
}

TEST_CASE("resume without -p is an error", "[client_args][resume]") {
    // Resume runs one headless turn; without -p there is nowhere for the prompt
    // to come from, and the TUI that would own an interactive resume does not
    // exist yet.
    const auto parsed = Parse({"--resume", "01ARZ3NDEKTSV4RRFFQ69G5FAV"});
    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("-p") != std::string::npos);
}

TEST_CASE("the resume and agent flags cannot be combined", "[client_args][resume]") {
    // A resumed session keeps the preset it was stored with; accepting both
    // would be a silent identity change of someone else's session.
    const auto parsed = Parse({"-p", "hi", "--resume", "01ARZ3", "--agent", "plan"});
    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("--agent") != std::string::npos);
}

TEST_CASE("the usage text documents --resume", "[client_args][resume]") {
    CHECK(UsageText().find("--resume") != std::string::npos);
}
