#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "fakes.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"
#include "command_scanner.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using tools_bash::BashSessionStateStore;
using tools_bash::BashSettings;
using tools_bash::ScanCommand;

namespace {

std::int64_t ElapsedMs(std::chrono::steady_clock::time_point from) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - from)
        .count();
}

struct Fixture {
    TempDir dir;
    BashSessionStateStore store;
    BashSettings settings;  // defaults: 120 s timeout, 1 MiB cap, /bin/sh

    struct Outcome {
        tools::ToolResult result;
        std::string streamed;  // everything emit_output received (the live channel)
        std::int64_t elapsed_ms = 0;
    };

    Outcome Run(const nlohmann::json& input, std::function<bool()> is_cancelled = {},
                const std::string& session_id = "s1") {
        auto tool = tools_bash::MakeBashTool(settings, store);
        Outcome out;
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = session_id;
        ctx.project_root = dir.path();
        ctx.is_cancelled = std::move(is_cancelled);
        ctx.emit_output = [&out](std::string_view chunk) { out.streamed += chunk; };
        const auto started = std::chrono::steady_clock::now();
        out.result = tool->execute(input, ctx);
        out.elapsed_ms = ElapsedMs(started);
        return out;
    }

    tools::PermissionDisclosure Disclose(const nlohmann::json& input,
                                         const std::string& session_id = "s1") {
        auto tool = tools_bash::MakeBashTool(settings, store);
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = session_id;
        ctx.project_root = dir.path();
        return tool->Disclose(input, ctx);
    }

    std::filesystem::path Write(const std::string& name, const std::string& content) {
        const auto path = dir.path() / name;
        std::ofstream out(path, std::ios::binary);
        out << content;
        return path;
    }

    std::filesystem::path Cwd(const std::string& session_id = "s1") {
        return store.Snapshot(session_id, dir.path()).cwd;
    }
};

bool Names(const tools_bash::ScanResult& result, const std::string& word) {
    return std::find(result.command_words.begin(), result.command_words.end(), word) !=
           result.command_words.end();
}

bool Offers(const tools::PermissionDisclosure& disclosure, const std::string& suffix) {
    return std::any_of(disclosure.grants.begin(), disclosure.grants.end(),
                       [&suffix](const tools::GrantOffer& offer) {
                           return offer.key.size() >= suffix.size() &&
                                  offer.key.compare(offer.key.size() - suffix.size(),
                                                    suffix.size(), suffix) == 0;
                       });
}

}  // namespace

// --- scanner (pure, no I/O) ------------------------------------------------------
//
// The scanner exists only to name what a prompt should disclose. It never
// rejects: the string reaches /bin/sh either way.

TEST_CASE("scanner names the program of each pipeline stage", "[tools_bash][scanner]") {
    const auto piped = ScanCommand("make 2>&1 | tail -50");
    CHECK(piped.complete);
    CHECK((piped.command_words == std::vector<std::string>{"make", "tail"}));

    const auto chained = ScanCommand("echo x > out.txt && cat out.txt");
    CHECK(chained.complete);
    CHECK((chained.command_words == std::vector<std::string>{"echo", "cat"}));

    // Redirection operands are filenames, never programs.
    CHECK_FALSE(Names(chained, "out.txt"));

    const auto assigned = ScanCommand("FOO=1 BAR=2 make install");
    CHECK(assigned.complete);
    CHECK((assigned.command_words == std::vector<std::string>{"make"}));
}

TEST_CASE("scanner does not split inside quotes", "[tools_bash][scanner]") {
    const auto result = ScanCommand("echo 'a;b|c' | tr a-z A-Z");
    CHECK(result.complete);
    CHECK((result.command_words == std::vector<std::string>{"echo", "tr"}));
}

TEST_CASE("scanner recurses into command substitution", "[tools_bash][scanner]") {
    // The regression this guards: treating $( ) as a blind spot would strand
    // `make -j$(nproc)` -- an entirely ordinary command -- in permanent
    // re-prompting, because it could never satisfy the grant conjunction.
    const auto arith = ScanCommand("make -j$(nproc)");
    CHECK(arith.complete);
    CHECK((arith.command_words == std::vector<std::string>{"make", "nproc"}));

    const auto backtick = ScanCommand("echo `id`");
    CHECK(backtick.complete);
    CHECK((backtick.command_words == std::vector<std::string>{"echo", "id"}));

    // ${...} is parameter expansion, not a command context.
    const auto param = ScanCommand("echo \"${HOME}/x\" | wc -c");
    CHECK(param.complete);
    CHECK((param.command_words == std::vector<std::string>{"echo", "wc"}));
}

TEST_CASE("scanner reports incomplete when the program is not a literal",
          "[tools_bash][scanner]") {
    // Not a list of scary programs -- the criterion is whether anything CAN be
    // named. `$X` and `eval` have no program to resolve or grant.
    CHECK_FALSE(ScanCommand("$X --flag").complete);
    CHECK(ScanCommand("$X --flag").command_words.empty());
    CHECK_FALSE(ScanCommand("eval \"$CMD\"").complete);
    CHECK_FALSE(ScanCommand(". ./script.sh").complete);
    CHECK_FALSE(ScanCommand("").complete);
    CHECK_FALSE(ScanCommand("   ").complete);
}

TEST_CASE("scanner names an interpreter without special-casing it", "[tools_bash][scanner]") {
    // `bash -c '...'` HAS a program: bash. It is named like any other, so a grant
    // on /bin/bash covers it. Refusing to honour that while honouring a grant on
    // git would be an interpreter denylist -- the same theatre as an executable
    // allowlist, and defeated the same way.
    const auto result = ScanCommand("bash -c 'rm -rf /'");
    CHECK(result.complete);
    CHECK((result.command_words == std::vector<std::string>{"bash"}));
}

TEST_CASE("scanner disclosure is a lower bound", "[tools_bash][scanner]") {
    // Documenting the accepted limit rather than pretending it away: semantic
    // indirection is invisible to any parser, so the prompt wording must not
    // imply the list is exhaustive.
    const auto result = ScanCommand("find . -name '*.o' | xargs rm");
    CHECK(result.complete);
    CHECK((result.command_words == std::vector<std::string>{"find", "xargs"}));
    CHECK_FALSE(Names(result, "rm"));  // rm is xargs' argument, not a command word
}

// --- settings ----------------------------------------------------------------------

TEST_CASE("bash settings take defaults when absent or nonsensical", "[tools_bash][settings]") {
    const auto defaults = tools_bash::ParseBashSettings(nlohmann::json::object());
    CHECK(defaults.timeout_ms == 120000);
    CHECK(defaults.max_output_bytes == 1024 * 1024);
    CHECK(defaults.shell == "/bin/sh");

    const nlohmann::json configured = {
        {"bash", {{"timeout_ms", 5000}, {"max_output_bytes", 64}, {"shell", "/bin/dash"}}}};
    const auto settings = tools_bash::ParseBashSettings(configured);
    CHECK(settings.timeout_ms == 5000);
    CHECK(settings.max_output_bytes == 64);
    CHECK(settings.shell == "/bin/dash");

    const nlohmann::json nonsense = {{"bash", {{"timeout_ms", 0}, {"max_output_bytes", -1}}}};
    const auto fallback = tools_bash::ParseBashSettings(nonsense);
    CHECK(fallback.timeout_ms == 120000);
    CHECK(fallback.max_output_bytes == 1024 * 1024);

    // A ceiling below the default would silently shorten every command that did
    // not ask for a timeout, which is not what configuring a maximum means.
    const nlohmann::json inverted = {{"bash", {{"timeout_ms", 90000}, {"max_timeout_ms", 1000}}}};
    CHECK(tools_bash::ParseBashSettings(inverted).max_timeout_ms == 90000);
}

// --- execution: the capability this rework exists for -------------------------------

TEST_CASE("bash runs pipelines, chains and redirection", "[tools_bash][exec]") {
    Fixture f;

    SECTION("pipe") {
        const auto out = f.Run({{"command", "echo hello | tr a-z A-Z"}});
        CHECK_FALSE(out.result.is_error);
        CHECK(out.result.content_text == "HELLO\n");
    }
    SECTION("&& chains") {
        const auto out = f.Run({{"command", "true && echo chained"}});
        CHECK_FALSE(out.result.is_error);
        CHECK(out.result.content_text == "chained\n");
    }
    SECTION("|| falls back") {
        const auto out = f.Run({{"command", "false || echo fallback"}});
        CHECK_FALSE(out.result.is_error);
        CHECK(out.result.content_text == "fallback\n");
    }
    SECTION("redirection") {
        const auto out = f.Run({{"command", "echo written > f.txt && cat f.txt"}});
        CHECK_FALSE(out.result.is_error);
        CHECK(out.result.content_text == "written\n");
    }
    SECTION("$VAR expands") {
        const auto out = f.Run({{"command", "export V=expanded && echo $V"}});
        CHECK_FALSE(out.result.is_error);
        CHECK(out.result.content_text == "expanded\n");
    }
}

TEST_CASE("bash merges stderr and reports a non-zero exit", "[tools_bash][exec]") {
    Fixture f;
    const auto out = f.Run({{"command", "ls definitely-not-a-directory-xyz"}});
    CHECK(out.result.is_error);
    CHECK(out.result.content_text.find("exit status: 1") != std::string::npos);
    CHECK(out.result.content_text.find("definitely-not-a-directory-xyz") != std::string::npos);
}

// --- session state round-trip -------------------------------------------------------

TEST_CASE("cd and export persist across calls via the state channel", "[tools_bash][state]") {
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub" / "deeper");
    f.Write("sub/deeper/leaf.txt", "leaf\n");

    REQUIRE_FALSE(f.Run({{"command", "cd sub && export MINIMAL_TEST_VAR=kept"}}).result.is_error);

    // A separate process, yet it starts where the previous one ended.
    const auto pwd = f.Run({{"command", "pwd -P"}});
    CHECK(pwd.result.content_text ==
          (std::filesystem::canonical(f.dir.path()) / "sub").string() + "\n");

    const auto env = f.Run({{"command", "printenv MINIMAL_TEST_VAR"}});
    CHECK_FALSE(env.result.is_error);
    CHECK(env.result.content_text == "kept\n");

    // Relative movement composes from where the session IS.
    REQUIRE_FALSE(f.Run({{"command", "cd deeper"}}).result.is_error);
    CHECK(f.Run({{"command", "cat leaf.txt"}}).result.content_text == "leaf\n");
}

TEST_CASE("unset is reflected, not merged away", "[tools_bash][state]") {
    // The reason state is adopted wholesale rather than merged: a merge would
    // resurrect variables the command deliberately removed.
    Fixture f;
    REQUIRE_FALSE(f.Run({{"command", "export GONE=1"}}).result.is_error);
    REQUIRE_FALSE(f.Run({{"command", "unset GONE"}}).result.is_error);
    CHECK(f.Run({{"command", "printenv GONE"}}).result.is_error);
}

TEST_CASE("a command that exits early leaves state unchanged", "[tools_bash][state]") {
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    const auto before = f.Cwd();

    // `exit` skips the trailer entirely, so nothing is reported and the previous
    // state must simply survive.
    const auto out = f.Run({{"command", "cd sub; exit 3"}});
    CHECK(out.result.is_error);
    CHECK(out.result.content_text.find("exit status: 3") != std::string::npos);
    CHECK(f.Cwd() == before);
}

TEST_CASE("a corrupted state payload leaves state unchanged", "[tools_bash][state]") {
    // A command can write to fd 9 itself, by accident or otherwise. Session state
    // must not be corruptible that way, so the payload is validated rather than
    // trusted -- and the command's own output still comes back normally.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    const auto before = f.Cwd();

    const auto out = f.Run({{"command", "cd sub && printf garbage >&9 && echo done"}});
    CHECK_FALSE(out.result.is_error);
    CHECK(out.result.content_text == "done\n");
    CHECK(f.Cwd() == before);  // the cd is discarded along with the bad payload
}

// --- timeout, cancellation, and bounded reaping --------------------------------------

TEST_CASE("timeout kills the process group and reports it", "[tools_bash][timeout]") {
    Fixture f;
    f.settings.timeout_ms = 300;

    const auto out = f.Run({{"command", "sleep 10"}});

    CHECK(out.result.is_error);
    CHECK(out.result.content_text.find("timed out after 300 ms") != std::string::npos);
    CHECK(out.elapsed_ms >= 290);   // the deadline is a hard floor on when we may kill
    CHECK(out.elapsed_ms < 5000);   // and the command's own 10 s never governs
}

TEST_CASE("a command already cancelled at entry returns immediately", "[tools_bash][cancel]") {
    // Regression. The child sets its own pgid via setsid(); before that runs, its
    // group is still ours, so killpg(pid) finds no such group and fails ESRCH. A
    // blocking waitpid then parked this thread for the command's natural life --
    // measured at 8008 ms for `sleep 8` against 306 ms when cancellation arrived
    // a moment later.
    Fixture f;
    f.settings.timeout_ms = 30000;

    const auto out = f.Run({{"command", "sleep 8"}}, [] { return true; });

    CHECK(out.result.is_error);
    CHECK(out.result.content_text.find("cancelled") != std::string::npos);
    CHECK(out.elapsed_ms < 2000);
}

TEST_CASE("cancellation mid-flight kills the process", "[tools_bash][cancel]") {
    Fixture f;
    f.settings.timeout_ms = 30000;

    std::atomic<bool> cancel{false};
    std::thread flipper([&cancel] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel = true;
    });
    const auto out = f.Run({{"command", "sleep 10"}}, [&cancel] { return cancel.load(); });
    flipper.join();

    CHECK(out.result.is_error);
    CHECK(out.result.content_text.find("cancelled") != std::string::npos);
    CHECK(out.elapsed_ms < 5000);
}

TEST_CASE("a command reading stdin does not hang", "[tools_bash][timeout]") {
    // Regression: with stdin inherited from the server rather than /dev/null,
    // `cat` blocked until the deadline and competed for the operator's keystrokes.
    Fixture f;
    f.settings.timeout_ms = 4000;

    const auto out = f.Run({{"command", "cat"}});

    CHECK_FALSE(out.result.is_error);
    CHECK(out.elapsed_ms < 2000);
}

TEST_CASE("closing stdout does not escape the deadline", "[tools_bash][timeout]") {
    // Regression: EOF on the pipes does not mean the child is gone. Reaping had
    // to become deadline-bounded, or a process that closes its stdio and keeps
    // working would hold the session worker for its full natural life.
    Fixture f;
    f.settings.timeout_ms = 600;

    const auto out = f.Run({{"command", "exec 1>&- 2>&- 9>&-; sleep 8"}});

    CHECK(out.result.is_error);
    CHECK(out.elapsed_ms < 4000);
}

TEST_CASE("timeout_ms is honoured and clamped to the ceiling", "[tools_bash][timeout]") {
    Fixture f;
    f.settings.timeout_ms = 30000;
    f.settings.max_timeout_ms = 400;

    SECTION("the model's shorter value wins") {
        const auto out = f.Run({{"command", "sleep 8"}, {"timeout_ms", 300}});
        CHECK(out.result.content_text.find("timed out after 300 ms") != std::string::npos);
    }
    SECTION("a value past the ceiling is clamped, not honoured") {
        const auto out = f.Run({{"command", "sleep 8"}, {"timeout_ms", 60000}});
        CHECK(out.result.content_text.find("timed out after 400 ms") != std::string::npos);
        CHECK(out.elapsed_ms < 4000);
    }
    SECTION("a non-positive value is rejected rather than silently clamped") {
        const auto out = f.Run({{"command", "echo x"}, {"timeout_ms", 0}});
        CHECK(out.result.is_error);
        CHECK(out.result.content_text.find("must be positive") != std::string::npos);
    }
}

// --- restart --------------------------------------------------------------------------

TEST_CASE("restart resets cwd and environment before running", "[tools_bash][restart]") {
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    REQUIRE_FALSE(f.Run({{"command", "cd sub && export MINIMAL_TEST_VAR=x"}}).result.is_error);

    const auto pwd = f.Run({{"command", "pwd -P"}, {"restart", true}});
    CHECK(pwd.result.content_text == std::filesystem::canonical(f.dir.path()).string() + "\n");
    CHECK(f.Run({{"command", "printenv MINIMAL_TEST_VAR"}}).result.is_error);
}

TEST_CASE("a rejected invocation does not reset state on its way out",
          "[tools_bash][restart]") {
    // restart used to be applied before the input was validated, so a malformed
    // call still wiped the session's cwd and environment.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    REQUIRE_FALSE(f.Run({{"command", "cd sub"}}).result.is_error);
    const auto after_cd = f.Cwd();

    const auto out = f.Run({{"command", "echo x"}, {"restart", true}, {"timeout_ms", "nope"}});

    CHECK(out.result.is_error);
    CHECK(f.Cwd() == after_cd);
}

// --- output cap / streaming ------------------------------------------------------------

TEST_CASE("persisted output is capped head+tail; the live stream is not",
          "[tools_bash][output]") {
    Fixture f;
    f.settings.max_output_bytes = 100;

    std::string big;
    for (int i = 0; i < 200; ++i) big += "line-" + std::to_string(i) + "\n";  // ~1.6 KB
    f.Write("big.txt", big);

    const auto out = f.Run({{"command", "cat big.txt"}});

    CHECK_FALSE(out.result.is_error);
    CHECK(out.result.content_text.find("bytes omitted") != std::string::npos);
    CHECK(out.result.content_text.size() < 100 + 64);  // cap plus the marker
    CHECK(out.result.content_text.rfind("line-0", 0) == 0);          // head survived
    CHECK(out.result.content_text.find("line-199") != std::string::npos);  // tail too

    CHECK(out.streamed == big);  // the live channel saw everything, uncapped
}

// --- disclosure -------------------------------------------------------------------------

TEST_CASE("disclosure names every stage by resolved path", "[tools_bash][disclosure]") {
    Fixture f;
    const auto disclosure = f.Disclose({{"command", "ls | wc -l"}});

    CHECK(disclosure.complete);
    CHECK(disclosure.grants.size() == 2);
    CHECK(Offers(disclosure, "/ls"));
    CHECK(Offers(disclosure, "/wc"));
    // Resolved, not bare: the prompt must show WHICH binary, since PATH is
    // session-mutable and `cp /bin/bash /tmp/ls` is a rename away.
    for (const auto& offer : disclosure.grants) {
        CHECK(offer.key.rfind("bash:/", 0) == 0);
    }
    CHECK(disclosure.summary.find("ls | wc -l") != std::string::npos);
}

TEST_CASE("disclosure follows the session PATH, not the server's",
          "[tools_bash][disclosure]") {
    // The rename trick cannot be prevented at this layer -- nothing here prevents
    // anything -- but it can be made visible at the moment consent is given,
    // which is the only place it can be.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "bin");
    const auto shim = f.dir.path() / "bin" / "git";
    std::ofstream(shim) << "#!/bin/sh\necho impostor\n";
    std::filesystem::permissions(shim, std::filesystem::perms::owner_all);

    REQUIRE_FALSE(f.Run({{"command", "export PATH=" + (f.dir.path() / "bin").string() +
                                         ":$PATH"}})
                      .result.is_error);

    const auto disclosure = f.Disclose({{"command", "git status"}});
    REQUIRE(disclosure.grants.size() == 1);
    CHECK(disclosure.grants.front().key ==
          "bash:" + std::filesystem::canonical(f.dir.path()).string() + "/bin/git");
}

TEST_CASE("a word that resolves to no executable contributes no grant",
          "[tools_bash][disclosure]") {
    // `export` is a builtin with no binary anywhere on PATH, so there is nothing
    // to grant. Resolution answers this without a builtin table -- which would
    // have to track the *configured* shell's builtin set to be correct, and is
    // dangerous precisely when it is wrong in the permissive direction.
    Fixture f;
    const auto disclosure = f.Disclose({{"command", "export FOO=1 && ls"}});

    CHECK(disclosure.complete);
    REQUIRE(disclosure.grants.size() == 1);
    CHECK(Offers(disclosure, "/ls"));
}

TEST_CASE("resolution errs toward over-disclosure, never under", "[tools_bash][disclosure]") {
    // macOS ships /usr/bin/cd, so `cd` names a binary the shell will never
    // actually exec. Pinning the behaviour because the DIRECTION is what matters:
    // over-disclosing only makes the conjunction stricter -- more keys to hold,
    // never fewer -- whereas a builtin table that wrongly dropped a real program
    // would make it looser.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    const auto disclosure = f.Disclose({{"command", "cd sub && ls"}});

    CHECK(disclosure.complete);
    CHECK(Offers(disclosure, "/ls"));
    CHECK(disclosure.grants.size() >= 1);
}

TEST_CASE("an unnameable program discloses no grants and is incomplete",
          "[tools_bash][disclosure]") {
    Fixture f;
    const auto disclosure = f.Disclose({{"command", "eval \"$CMD\""}});
    CHECK_FALSE(disclosure.complete);
    CHECK(disclosure.grants.empty());
}

// --- the real tool through the real gate ---------------------------------------------------

TEST_CASE("granting a real pipeline silences the prompt for the next one",
          "[tools_bash][disclosure][integration]") {
    // The seam the unit tests on either side cannot see: BashTool builds grant
    // keys and ToolExecutor matches them. If those two ever disagreed on format,
    // grants would silently never apply and every invocation would re-prompt.
    Fixture f;
    tools::ToolRegistry registry;
    registry.Register(tools_bash::MakeBashTool(f.settings, f.store));

    testing::GrantingPermissionChecker checker;
    tools::PermissionPolicy policy;
    policy.execute = tools::PermissionMode::Ask;
    tools::ToolExecutor executor(registry, checker, policy, {}, tools::PermissionRunMode::Normal);

    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";
    ctx.project_root = f.dir.path();

    const auto first = executor.Run("tu1", "bash", {{"command", "ls | wc -l"}}, ctx);
    CHECK_FALSE(first.is_error);
    REQUIRE(checker.seen.size() == 1);
    CHECK(checker.seen.front().disclosure.grants.size() == 2);  // ls and wc

    // Same two binaries, different arguments: covered by the grants just made.
    const auto second = executor.Run("tu2", "bash", {{"command", "ls -a | wc -c"}}, ctx);
    CHECK_FALSE(second.is_error);
    CHECK(checker.seen.size() == 1);

    // A third binary was never disclosed before, so it must ask again.
    const auto third = executor.Run("tu3", "bash", {{"command", "ls | sort | wc -l"}}, ctx);
    CHECK_FALSE(third.is_error);
    CHECK(checker.seen.size() == 2);
}

// --- session isolation ------------------------------------------------------------------

TEST_CASE("sessions keep independent cwd and environment", "[tools_bash][sessions]") {
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");

    REQUIRE_FALSE(
        f.Run({{"command", "cd sub && export MINIMAL_TEST_VAR=s1only"}}, {}, "s1").result.is_error);

    const auto s2_pwd = f.Run({{"command", "pwd -P"}}, {}, "s2");
    CHECK(s2_pwd.result.content_text == std::filesystem::canonical(f.dir.path()).string() + "\n");
    CHECK(f.Run({{"command", "printenv MINIMAL_TEST_VAR"}}, {}, "s2").result.is_error);

    // Erase frees the entry (the future OnSessionEnded path).
    f.store.Erase("s1");
    CHECK(f.store.Snapshot("s1", f.dir.path()).env.count("MINIMAL_TEST_VAR") == 0);
}

TEST_CASE("/restart-shell resets the same state `restart: true` does",
          "[tools_bash][commands][invariant]") {
    // The point of the plugin-owns-its-commands rule, asserted rather than
    // asserted about: bash's per-session cwd/env has ONE owner, and the tool
    // flag, the command and the session observer all reach it through that owner
    // rather than each keeping their own idea of the state.
    minimal::tools_bash::BashSessionStateStore store;
    const std::filesystem::path root = std::filesystem::temp_directory_path();
    store.Snapshot("s1", root);
    store.SetState("s1", root / "sub", {{"KEEP", "yes"}});
    store.Snapshot("s2", root);
    REQUIRE(store.Has("s1"));
    REQUIRE(store.size() == 2);

    // What /restart-shell does. Erase, not Reset: the next Snapshot re-seeds
    // from project_root, which is the same end state and one fewer code path.
    store.Erase("s1");

    CHECK_FALSE(store.Has("s1"));
    CHECK(store.Has("s2"));  // only that session's, as OnSessionEnded also requires
    CHECK(store.size() == 1);
}
