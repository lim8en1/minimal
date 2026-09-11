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
#include <catch2/interfaces/catch_interfaces_testcase.hpp>
#include <catch2/internal/catch_context.hpp>
#include <catch2/internal/catch_test_case_registry_impl.hpp>
#include <catch2/catch_test_case_info.hpp>

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

TEST_CASE("a bare --server word is a clean usage error, not an abort",
          "[client_args]") {
    // ResolveServerTarget throws for a non-connectable bare word (it would
    // resolve as DNS and never reach a local socket) -- ParseArgs must turn
    // that into the same Action::Error every other bad flag gets, not let it
    // escape uncaught.
    const auto parsed = Parse({"--server", "myserver"});
    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("myserver") != std::string::npos);
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

TEST_CASE("the model flag is validated before a server is contacted", "[client_args]") {
    // The shape is decidable locally, and failing here keeps a typo from
    // presenting as an unreachable server -- which is what the -p defect above
    // did, and took a while to see for exactly that reason.
    SECTION("a well-formed reference is kept verbatim") {
        const auto parsed = Parse({"-p", "hi", "--model", "local/qwen4"});
        REQUIRE(parsed.action == ParsedArgs::Action::Headless);
        CHECK(parsed.headless.model == "local/qwen4");
    }

    SECTION("a bare model name is refused, and the message shows the form") {
        const auto parsed = Parse({"-p", "hi", "--model", "qwen4"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("<provider>/<model>") != std::string::npos);
        CHECK(parsed.error.find("local/qwen4") != std::string::npos);
    }

    SECTION("half a reference is refused") {
        CHECK(Parse({"-p", "hi", "--model", "local/"}).action == ParsedArgs::Action::Error);
        CHECK(Parse({"-p", "hi", "--model", "/qwen4"}).action == ParsedArgs::Action::Error);
    }

    SECTION("a nested id is accepted: the first '/' splits, later ones are the model") {
        // Discovered catalogs serve slashy ids (openrouter/anthropic/claude-*).
        // A mis-split ref resolves to nothing server-side and is reported as
        // "unknown model" -- strictness here would only make real refs
        // unaddressable.
        const auto parsed = Parse({"-p", "hi", "--model", "a/b/c"});
        REQUIRE(parsed.action == ParsedArgs::Action::Headless);
        CHECK(parsed.headless.model == "a/b/c");
    }

    SECTION("no --model leaves it to the preset") {
        CHECK(Parse({"-p", "hi"}).headless.model.empty());
    }
}

TEST_CASE("no test case name starts with a dash", "[client_args][meta]") {
    // A NOTE WAS NOT ENOUGH. catch_discover_tests registers each case by passing
    // its name to the binary as an argv token, so a name beginning with '-' is
    // read back as a flag: the suite passes when run directly and fails under
    // ctest, with an error about an unrecognised option rather than about the
    // test. This has now happened twice while writing tests ABOUT argument
    // parsing, which is exactly the kind of thing a comment does not prevent.
    for (const auto& info : Catch::getAllTestCasesSorted(*Catch::getCurrentContext().getConfig())) {
        INFO("test case: " << info.getTestCaseInfo().name);
        CHECK(info.getTestCaseInfo().name.front() != '-');
    }
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

TEST_CASE("resume without -p opens the TUI on the stored session", "[client_args][resume]") {
    // Used to be an error ("--resume needs -p"); the TUI startup resume path
    // made the flag meaningful without a prompt: attach to the session when
    // it is live, revive it from the transcript when it is not.
    const auto parsed = Parse({"--resume", "01ARZ3NDEKTSV4RRFFQ69G5FAV"});
    REQUIRE(parsed.action == ParsedArgs::Action::Tui);
    CHECK(parsed.headless.resume_session_id == "01ARZ3NDEKTSV4RRFFQ69G5FAV");
}

TEST_CASE("TUI resume refuses the per-turn override flags", "[client_args][resume]") {
    // The stored row's preset/model govern a resumed session, and a TUI
    // resume has no single turn to borrow --agent/--model FOR -- a headless
    // resume does (the two tests above/below its TurnOverrides path).
    SECTION("--agent") {
        const auto parsed = Parse({"--resume", "01ARZ3", "--agent", "plan"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("--agent") != std::string::npos);
    }
    SECTION("--model") {
        const auto parsed = Parse({"--resume", "01ARZ3", "--model", "local/qwen4"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("--model") != std::string::npos);
    }
}

TEST_CASE("resume and agent flags combine: the agent borrows the resumed turn only", "[client_args][resume]") {
    // Used to be an error ("a resumed session keeps the preset it was stored
    // with"); the per-turn override path made --agent on a resume meaningful
    // without an identity change: the stored row's preset still governs the
    // session, and the named preset applies to the one turn a headless resume
    // runs (RunTurn's TurnOverrides).
    const auto parsed = Parse({"-p", "hi", "--resume", "01ARZ3", "--agent", "plan"});
    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    CHECK(parsed.headless.resume_session_id == "01ARZ3");
    CHECK(parsed.headless.agent_preset == "plan");
}

TEST_CASE("the usage text documents --resume", "[client_args][resume]") {
    CHECK(UsageText().find("--resume") != std::string::npos);
}

TEST_CASE("the --signals flag parses a comma-separated list and refuses empties", "[client_args][signals]") {
    const auto parsed = Parse({"-p", "watch the builds", "--signals", "ci/build.completed, reminder, sessions"});
    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    const auto& signals = parsed.headless.signals;
    REQUIRE(signals.size() == 3);
    CHECK(signals[0] == "ci/build.completed");
    CHECK(signals[1] == "reminder");
    CHECK(signals[2] == "sessions");

    // Trailing comma: an empty selector, refused rather than skipped.
    const auto trailing = Parse({"-p", "x", "--signals", "ci/build.completed,"});
    CHECK(trailing.action == ParsedArgs::Action::Error);
    CHECK(trailing.error.find("empty selector") != std::string::npos);

    // Inline form works, like every value-taking flag.
    const auto inline_form = Parse({"-p", "x", "--signals=sessions"});
    REQUIRE(inline_form.action == ParsedArgs::Action::Headless);
    CHECK(inline_form.headless.signals == std::vector<std::string>{"sessions"});
}

TEST_CASE("the --signals flag without -p is an error, like --resume", "[client_args][signals]") {
    const auto parsed = Parse({"--signals", "sessions"});
    REQUIRE(parsed.action == ParsedArgs::Action::Error);
    CHECK(parsed.error.find("--signals needs -p") != std::string::npos);
}

TEST_CASE("auto-spawn defaults on and --no-autospawn turns it off", "[client_args]") {
    CHECK(Parse({"-p", "hi"}).headless.autospawn);
    const auto off = Parse({"-p", "hi", "--no-autospawn"});
    REQUIRE(off.action == ParsedArgs::Action::Headless);
    CHECK_FALSE(off.headless.autospawn);

    // The flag exists so a script never blocks on a startup: the usage text has
    // to be where they find it.
    CHECK(UsageText().find("--no-autospawn") != std::string::npos);
}

// --- List sessions -------------------------------------------------------------

TEST_CASE("list-sessions is its own action", "[client_args][list]") {
    const auto parsed = Parse({"--list-sessions"});
    REQUIRE(parsed.action == ParsedArgs::Action::ListSessions);
    CHECK_FALSE(parsed.headless.all_dirs);

    const auto all = Parse({"--list-sessions", "--all-dirs"});
    REQUIRE(all.action == ParsedArgs::Action::ListSessions);
    CHECK(all.headless.all_dirs);
}

TEST_CASE("list-sessions refuses the flags it has no use for", "[client_args][list]") {
    SECTION("-p") {
        const auto parsed = Parse({"--list-sessions", "-p", "hi"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("-p") != std::string::npos);
    }
    SECTION("--resume") {
        const auto parsed = Parse({"--list-sessions", "--resume", "01ARZ3"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("--resume") != std::string::npos);
    }
    SECTION("session-shaping flags") {
        CHECK(Parse({"--list-sessions", "--agent", "plan"}).action == ParsedArgs::Action::Error);
        CHECK(Parse({"--list-sessions", "--model", "local/qwen4"}).action ==
              ParsedArgs::Action::Error);
        CHECK(Parse({"--list-sessions", "--signals", "sessions"}).action ==
              ParsedArgs::Action::Error);
    }
}

TEST_CASE("output-format json belongs to list-sessions", "[client_args][list]") {
    SECTION("accepted alongside it") {
        const auto parsed = Parse({"--list-sessions", "--output-format", "json"});
        REQUIRE(parsed.action == ParsedArgs::Action::ListSessions);
        CHECK(parsed.headless.list_json);
    }
    SECTION("refused on a turn") {
        // A turn's output formats stay text|jsonl; json would otherwise look
        // accepted and silently mean jsonl.
        const auto parsed = Parse({"-p", "hi", "--output-format", "json"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("--list-sessions") != std::string::npos);
    }
    SECTION("jsonl refused on a listing") {
        const auto parsed = Parse({"--list-sessions", "--output-format", "jsonl"});
        REQUIRE(parsed.action == ParsedArgs::Action::Error);
        CHECK(parsed.error.find("json") != std::string::npos);
    }
}

TEST_CASE("a -p prompt with invalid UTF-8 is sanitized, not sent raw",
          "[client_args][invariant]") {
    // "caf\xE9" -- 0xE9 alone (Latin-1 'e') is not a valid UTF-8 lead byte for
    // a 1-byte sequence and has no continuation byte following: malformed.
    // CreateSessionRequest.name/RunTurn's prompt are proto3 strings the
    // SERVER rejects outright on invalid UTF-8 -- sanitize here instead of a
    // confusing INTERNAL error deep in the turn.
    const auto parsed = Parse({"-p", "caf\xE9"});
    REQUIRE(parsed.action == ParsedArgs::Action::Headless);
    CHECK(parsed.headless.prompt == "caf\xEF\xBF\xBD");
}
