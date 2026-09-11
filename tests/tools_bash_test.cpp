#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>
#include <vector>

#include "fakes.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

#include "background_registry.hpp"
#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"
#include "command_scanner.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using minimal::testing::TempDir;
using tools_bash::BackgroundTaskRegistry;
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
    BackgroundTaskRegistry background;
    BashSettings settings;  // defaults: 120 s timeout, 1 MiB cap, /bin/sh

    struct Outcome {
        tools::ToolResult result;
        std::string streamed;  // everything emit_output received (the live channel)
        std::int64_t elapsed_ms = 0;
    };

    Outcome Run(const nlohmann::json& input, std::function<bool()> is_cancelled = {},
                const std::string& session_id = "s1") {
        auto tool = tools_bash::MakeBashTool(settings, store, background);
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
        auto tool = tools_bash::MakeBashTool(settings, store, background);
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

// --- display payload -------------------------------------------------------------

TEST_CASE("bash results carry a process display payload with the run's facts",
          "[tools_bash][display]") {
    Fixture f;
    SECTION("success") {
        const auto out = f.Run({{"command", "echo hi"}});
        const auto& display = out.result.display;
        REQUIRE(display.at("kind") == "process");
        CHECK(display.at("command") == "echo hi");
        CHECK(display.at("exit_code") == 0);
        CHECK(display.at("output") == "hi\n");
        CHECK(display.at("output_truncated") == false);
        CHECK(display.at("cancelled") == false);
        CHECK(display.at("timed_out") == false);
        CHECK(display.at("duration_ms").is_number_integer());
        CHECK_FALSE(display.at("cwd").get<std::string>().empty());
    }
    SECTION("a non-zero exit is a field; the footer stays in the model's text only") {
        const auto out = f.Run({{"command", "echo oops; exit 3"}});
        CHECK(out.result.is_error);
        CHECK(out.result.content_text.find("exit status: 3") != std::string::npos);
        CHECK(out.result.display.at("exit_code") == 3);
        CHECK(out.result.display.at("output") == "oops\n");
    }
    SECTION("a long output keeps only its tail") {
        // No SIGPIPE'd producer (`yes | head` kills yes): exactly 20000 bytes.
        const auto out = f.Run({{"command", "head -c 20000 /dev/zero | tr '\\0' x"}});
        CHECK(out.result.content_text.size() == 20000);   // the model still gets it all
        CHECK(out.result.display.at("output_truncated") == true);
        CHECK(out.result.display.at("output").get<std::string>().size() == 16 * 1024);
    }
}

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

TEST_CASE("shell keywords do not consume the command slot", "[tools_bash][scanner]") {
    // THE REGRESSION, and it was a live authorisation hole rather than a missed
    // grant. `then` was recorded as the program of its list, which demoted `rm`
    // to an argument -- so `rm` was never disclosed, `complete` stayed true
    // because nothing had failed, and a session already holding a grant on the
    // harmless `ls` satisfied the whole conjunction and ran `rm -rf /` with no
    // prompt at all. See the invariant in command_scanner.hpp.
    const auto branch = ScanCommand("ls; if true; then rm -rf /; fi");
    CHECK(Names(branch, "rm"));
    CHECK_FALSE(Names(branch, "then"));
    CHECK_FALSE(Names(branch, "if"));
    CHECK_FALSE(Names(branch, "fi"));

    // The same slot-stealing shape in every other compound form.
    CHECK(Names(ScanCommand("for f in *.o; do rm $f; done"), "rm"));
    CHECK(Names(ScanCommand("while read -r l; do rm $l; done"), "rm"));
    CHECK(Names(ScanCommand("time rm -rf /"), "rm"));
    CHECK(Names(ScanCommand("! rm -rf /"), "rm"));
    CHECK(Names(ScanCommand("if [ -f x ]; then rm x; else mv y x; fi"), "mv"));

    // A keyword alone names nothing, and nothing nameable means incomplete --
    // it must not resolve to a complete, empty disclosure that grants itself.
    CHECK_FALSE(ScanCommand("fi").complete);
}

TEST_CASE("scanner treats $'...' as the literal word it is", "[tools_bash][scanner]") {
    // `$'` used to be consumed as a two-character `$x` token, leaving the
    // closing quote to open a fresh quote region that swallowed the rest of the
    // line -- so the pipeline stage after it vanished from the disclosure.
    const auto ansi = ScanCommand("grep $'\\t' file | wc -l");
    CHECK(ansi.complete);
    CHECK(Names(ansi, "grep"));
    CHECK(Names(ansi, "wc"));

    // A program spelled with ANSI-C quoting is still a literal program.
    const auto quoted = ScanCommand("$'rm' -rf /");
    CHECK(quoted.complete);
    CHECK((quoted.command_words == std::vector<std::string>{"rm"}));

    CHECK_FALSE(ScanCommand("grep $'unterminated").complete);
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

// --- scanner: the grammar ----------------------------------------------------------
//
// The tree-sitter-bash rewrite adds two guarantees the hand-rolled walker
// could not make: input the parser does not understand fails closed, and
// constructs that are data rather than commands are no longer scanned as
// commands. The tests above are the contract this half must keep unchanged.

TEST_CASE("scanner fails closed on input the grammar does not understand",
          "[tools_bash][scanner]") {
    // tree-sitter is error-recovering: it produces a tree for garbage rather
    // than refusing. ERROR and MISSING nodes are the only signal that it
    // guessed, and a guessed parse must not report itself complete -- that is
    // exactly how the reserved-word hole once authorized more than the
    // operator saw.
    CHECK_FALSE(ScanCommand("echo \"unclosed").complete);   // ERROR node
    CHECK_FALSE(ScanCommand("foo $( bar").complete);        // MISSING ')'
    CHECK_FALSE(ScanCommand("rm -rf /; if true; then").complete);  // MISSING fi

    // Failing closed does not mean failing usefully-blind: what DID parse is
    // still named, so a grant on the literal prefix can do its job once the
    // operator has seen the rest.
    CHECK(Names(ScanCommand("foo $( bar"), "foo"));
    CHECK(Names(ScanCommand("rm -rf /; if true; then"), "rm"));
}

TEST_CASE("heredoc bodies are data, not commands", "[tools_bash][scanner]") {
    // The hand-rolled walker scanned heredoc bodies as commands, so this
    // offered a grant for rm -- a program that only ever existed as an
    // echoed line of text.
    const auto result = ScanCommand("cat <<'EOF'\nrm -rf /\nEOF");
    CHECK(result.complete);
    CHECK((result.command_words == std::vector<std::string>{"cat"}));
    CHECK_FALSE(Names(result, "rm"));
}

TEST_CASE("scanner names command positions the grammar structures",
          "[tools_bash][scanner]") {
    // Function bodies, subshells, case arms: the grammar yields command_name
    // nodes at each, where the walker approximated or over-recorded.
    CHECK(Names(ScanCommand("f() { rm -rf /; }"), "rm"));
    CHECK(Names(ScanCommand("(cd /tmp && ls)"), "cd"));
    CHECK(Names(ScanCommand("(cd /tmp && ls)"), "ls"));
    CHECK(Names(ScanCommand("case $x in a) rm -rf /;; esac"), "rm"));

    // `coproc` is one of the two keywords the grammar leaves UNstructured --
    // it parses as a plain command with `{ rm -rf /; }` smeared into its
    // arguments -- so the reserved-word rule must recover the real program
    // from among them.
    CHECK(Names(ScanCommand("coproc thing { rm -rf /; }"), "rm"));

    // Conversely, positions that are NOT commands are no longer recorded as
    // programs. The old walker named the loop variable `f` (and `[`), each of
    // which could only ever contribute a junk grant offer.
    CHECK_FALSE(Names(ScanCommand("for f in *.o; do rm $f; done"), "f"));
    CHECK_FALSE(Names(ScanCommand("if [ -f x ]; then rm x; fi"), "["));
}

TEST_CASE("a computed program is incomplete even mid-word", "[tools_bash][scanner]") {
    // `foo$(nproc) bar`: the program is built by substitution, so there is no
    // literal to name. The old walker saw a word that merely BEGAN with a
    // literal and confidently recorded `foo` -- over-confident in exactly the
    // way `complete` exists to prevent. The substitution's own command is
    // still a literal and still named.
    const auto computed = ScanCommand("foo$(nproc) bar");
    CHECK_FALSE(computed.complete);
    CHECK(Names(computed, "nproc"));
    CHECK_FALSE(Names(computed, "foo"));

    CHECK_FALSE(ScanCommand("v$i").complete);
}

TEST_CASE("quoted programs are named unquoted", "[tools_bash][scanner]") {
    // The disclosure feeds PATH resolution, so the program a quoted spelling
    // invokes -- not its quote characters -- is what must be named.
    CHECK((ScanCommand("'ls' -la").command_words == std::vector<std::string>{"ls"}));
    CHECK((ScanCommand("\"echo\" hi").command_words == std::vector<std::string>{"echo"}));
    CHECK((ScanCommand("$'rm' -rf /").command_words == std::vector<std::string>{"rm"}));
}

// --- settings ----------------------------------------------------------------------

TEST_CASE("bash settings take defaults when absent or nonsensical", "[tools_bash][settings]") {
    const auto defaults = tools_bash::ParseBashSettings(nlohmann::json::object());
    CHECK(defaults.timeout_ms == 120000);
    CHECK(defaults.max_output_bytes == 1024 * 1024);
    CHECK(defaults.shell == "/bin/sh");

    // The plugin's own plugins.json "config" object, verbatim -- flat keys,
    // every one of them belonging to this tool.
    const nlohmann::json configured = {
        {"timeout_s", 5}, {"max_output_bytes", 64}, {"shell", "/bin/dash"}};
    const auto settings = tools_bash::ParseBashSettings(configured);
    CHECK(settings.timeout_ms == 5000);
    CHECK(settings.max_output_bytes == 64);
    CHECK(settings.shell == "/bin/dash");

    const nlohmann::json nonsense = {{"timeout_s", 0}, {"max_output_bytes", -1}};
    const auto fallback = tools_bash::ParseBashSettings(nonsense);
    CHECK(fallback.timeout_ms == 120000);
    CHECK(fallback.max_output_bytes == 1024 * 1024);

    // A ceiling below the default would silently shorten every command that did
    // not ask for a timeout, which is not what configuring a maximum means.
    const nlohmann::json inverted = {{"timeout_s", 90}, {"max_timeout_s", 1}};
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

TEST_CASE("a trailing backslash does not swallow the state trailer", "[tools_bash][state]") {
    // `echo hi \` + trailer is one logical line to the shell -- backslash-
    // newline is a continuation -- so the marker became an ARGUMENT and the
    // model saw "hi __minimal_rc=0" instead of "hi". Fails with the fix
    // reverted.
    Fixture f;
    const auto out = f.Run({{"command", "echo hi \\"}});
    CHECK_FALSE(out.result.is_error);
    CHECK(out.result.content_text == "hi\n");

    // State still round-trips across the continuation.
    std::filesystem::create_directories(f.dir.path() / "sub");
    REQUIRE_FALSE(f.Run({{"command", "cd sub && echo ok \\"}}).result.is_error);
    CHECK(f.Cwd() == std::filesystem::canonical(f.dir.path()) / "sub");
}

TEST_CASE("a directory named with a slash offers no grant", "[tools_bash][disclosure]") {
    // The slash branch of ResolveOnPath omitted is_regular_file, so
    // access(dir, X_OK) -- which succeeds for directories on POSIX -- offered
    // a grant for a path that cannot be a program. Over-disclosure only, but
    // disclosure is what consent is given against. Fails with the fix
    // reverted: the grants included the directory itself.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    const auto disclosure = f.Disclose({{"command",
                                         (f.dir.path() / "sub").string() + " && ls"}});
    CHECK(disclosure.complete);
    CHECK(Offers(disclosure, "/ls"));
    for (const auto& offer : disclosure.grants) {
        CHECK(offer.key.find("/sub") == std::string::npos);
    }
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

TEST_CASE("timeout is honoured and clamped to the ceiling", "[tools_bash][timeout]") {
    Fixture f;
    f.settings.timeout_ms = 30000;
    f.settings.max_timeout_ms = 2000;

    SECTION("the model's shorter value wins") {
        const auto out = f.Run({{"command", "sleep 8"}, {"timeout", 1}});
        CHECK(out.result.content_text.find("timed out after 1000 ms") != std::string::npos);
    }
    SECTION("a value past the ceiling is clamped, not honoured") {
        const auto out = f.Run({{"command", "sleep 8"}, {"timeout", 10}});
        CHECK(out.result.content_text.find("timed out after 2000 ms") != std::string::npos);
        CHECK(out.elapsed_ms < 4000);
    }
    SECTION("a non-positive value is rejected rather than silently clamped") {
        const auto out = f.Run({{"command", "echo x"}, {"timeout", 0}});
        CHECK(out.result.is_error);
        CHECK(out.result.content_text.find("must be positive") != std::string::npos);
    }
    SECTION("a value near INT64_MAX is rejected, not overflowed into a negative deadline") {
        // seconds * 1000 on an unchecked value near INT64_MAX wraps (signed
        // overflow, UB); the wrapped result used to sail past both
        // std::min(timeout_ms, max_timeout_ms) here and the background
        // path's equivalent, reaching the drain loop as a negative deadline.
        const auto out = f.Run(
            {{"command", "echo x"}, {"timeout", std::numeric_limits<std::int64_t>::max()}});
        CHECK(out.result.is_error);
        CHECK(out.result.content_text.find("out of range") != std::string::npos);
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

TEST_CASE("a deleted session cwd reports the reason, not a bare exit 127",
          "[tools_bash][restart]") {
    // `cd build && rm -rf build` from outside the session: the cwd no longer
    // exists and every later command fails at the child's chdir. The child's
    // diagnostic must reach the captured output -- the model's only channel --
    // which requires the stderr redirect to happen BEFORE the chdir in the
    // child; written earlier, the message landed on the server's own stderr
    // and the model saw a bare exit 127 in exactly the case where recovery
    // guidance matters.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    REQUIRE_FALSE(f.Run({{"command", "cd sub"}}).result.is_error);
    std::filesystem::remove_all(f.dir.path() / "sub");

    const auto out = f.Run({{"command", "echo hi"}});
    REQUIRE(out.result.is_error);
    CHECK(out.result.content_text.find("chdir to session cwd failed") != std::string::npos);
}

TEST_CASE("a rejected invocation does not reset state on its way out",
          "[tools_bash][restart]") {
    // restart used to be applied before the input was validated, so a malformed
    // call still wiped the session's cwd and environment.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    REQUIRE_FALSE(f.Run({{"command", "cd sub"}}).result.is_error);
    const auto after_cd = f.Cwd();

    const auto out = f.Run({{"command", "echo x"}, {"restart", true}, {"timeout", "nope"}});

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
    registry.Register(tools_bash::MakeBashTool(f.settings, f.store, f.background));

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

TEST_CASE("a keyword-hidden program is not silenced by an unrelated grant",
          "[tools_bash][disclosure][integration]") {
    // The whole hole, end to end. Disclose() executes nothing, so this half can
    // carry the real string: `rm` must reach the operator's prompt.
    Fixture f;
    const auto disclosure = f.Disclose({{"command", "ls; if true; then rm -rf /; fi"}});
    CHECK(disclosure.complete);
    CHECK(Offers(disclosure, "/rm"));
    CHECK(Offers(disclosure, "/ls"));

    // And the half that runs: a grant on `ls` must NOT satisfy the conjunction
    // for a second command hiding another program behind `then`. `sort` stands
    // in for `rm` here for the obvious reason; the scanner cannot tell them
    // apart, which is the point. Before the fix the disclosure named only `ls`,
    // reported itself complete, and this ran with checker.seen still at 1.
    tools::ToolRegistry registry;
    registry.Register(tools_bash::MakeBashTool(f.settings, f.store, f.background));

    testing::GrantingPermissionChecker checker;
    tools::PermissionPolicy policy;
    policy.execute = tools::PermissionMode::Ask;
    tools::ToolExecutor executor(registry, checker, policy, {}, tools::PermissionRunMode::Normal);

    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";
    ctx.project_root = f.dir.path();

    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "ls"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 1);

    CHECK_FALSE(
        executor.Run("tu2", "bash", {{"command", "ls; if true; then sort /dev/null; fi"}}, ctx)
            .is_error);
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

TEST_CASE("a flooded fd-9 payload is discarded wholesale, not truncated into adoption",
          "[tools_bash][state][invariant]") {
    // The state payload is kilobytes at worst. Unbounded, a command pumping fd 9
    // (`yes >&9`) grew one string for the whole timeout. This test drives the
    // overflow path and asserts the all-or-nothing rule: a flooded (necessarily
    // truncated) payload leaves cwd AND environment untouched. The byte cap
    // itself is not externally observable -- there is no seam through the
    // process-internal buffer -- so the memory bound is held by the constant,
    // and this test holds the observable half.
    Fixture f;
    std::filesystem::create_directories(f.dir.path() / "sub");
    const auto before = f.Cwd();
    REQUIRE_FALSE(f.Run({{"command", "export KEEP_ME=1"}}).result.is_error);

    const auto out = f.Run(
        {{"command", "yes 'junk' >&9 & true"}, {{"timeout", "1500"}}});

    CHECK(f.Cwd() == before);  // no state adopted from the flood
    // Previous state survived whole -- not merged away, not partially replaced.
    CHECK(f.store.Snapshot("s1", f.dir.path()).env.count("KEEP_ME") == 1);
}

TEST_CASE("the seeded shell environment is allowlisted; 'all' is an explicit choice",
          "[tools_bash][env][invariant]") {
    // The server process's environment carries every credential the process
    // holds -- provider API keys resolved from api_key_env -- and the seed
    // USED to be all of it: a model-authored `env` persisted those into the
    // transcript, where any later network-capable command could read them.
    // The allowlist is the invariant; "all" is the operator's opt-out.
    ::setenv("MINIMAL_SECRET_CANARY", "hunter2", 1);
    struct Restore {
        ~Restore() { ::unsetenv("MINIMAL_SECRET_CANARY"); }
    } restore;

    BashSessionStateStore filtered;
    const auto seed = filtered.Snapshot("s-env", std::filesystem::temp_directory_path());
    CHECK(seed.env.count("MINIMAL_SECRET_CANARY") == 0);
    CHECK(seed.env.count("PATH") == 1);   // what the model legitimately needs
    CHECK(seed.env.count("HOME") == 1);

    BashSessionStateStore all("all");
    const auto full = all.Snapshot("s-env-all", std::filesystem::temp_directory_path());
    CHECK(full.env.count("MINIMAL_SECRET_CANARY") == 1);
}

TEST_CASE("a typo'd inherit_env is a named parse error, not a silent default",
          "[tools_bash][env]") {
    // The two choices are named; "alowlist" silently meaning "allowlist" --
    // or worse, "all" -- is the config lie this plugin's parse ladder exists
    // to refuse.
    CHECK_THROWS_AS(tools_bash::ParseBashSettings(
                        nlohmann::json{{"inherit_env", "alowlist"}}),
                    std::runtime_error);
    CHECK_NOTHROW(tools_bash::ParseBashSettings(nlohmann::json{{"inherit_env", "all"}}));
    CHECK_NOTHROW(tools_bash::ParseBashSettings(nlohmann::json{{"inherit_env", "allowlist"}}));
}
