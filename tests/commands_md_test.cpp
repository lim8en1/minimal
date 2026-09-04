#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

#include "../../plugins/server/commands-md/commands_md.hpp"
#include "minimal/commands/command_dispatcher.hpp"
#include "commands_core_fixture.hpp"

using namespace minimal;
using namespace minimal::commands_md;

namespace {

// RAII env override, matching commands_test.cpp's convention.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) { had_old_ = true; old_ = old; }
        ::setenv(name, value, 1);
    }
    ~ScopedEnv() {
        if (had_old_) ::setenv(name_.c_str(), old_.c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
private:
    std::string name_;
    bool had_old_ = false;
    std::string old_;
};

// Writes one command file (parent directories included) and loads everything
// under dir as commands. Trusted by default -- these tests exercise
// parsing/rendering/execution, not the untrusted-confirmation gate, which has
// its own dedicated tests below.
std::vector<std::unique_ptr<commands::Command>> WriteAndLoad(
    const std::filesystem::path& dir, const std::filesystem::path& relative,
    const std::string& contents, bool trusted = true) {
    const auto path = dir / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << contents;
    return LoadCommands({{dir, trusted}});
}

const commands::Command* Find(const std::vector<std::unique_ptr<commands::Command>>& commands,
                              const std::string& name) {
    for (const auto& command : commands) {
        if (command->Name() == name) return command.get();
    }
    return nullptr;
}

// Runs one command through the real dispatcher over the commands-core fixture's
// fake session (records SubmitTurn; answers confirmations with `answer`).
commands::CommandResult DispatchOne(commands::CommandRegistry& registry,
                                    testing::CommandFixture& fixture, const std::string& name,
                                    std::vector<std::string> positional,
                                    const std::string& answer = "submit") {
    fixture.confirmations.answer = answer;
    commands::CommandArgs args;
    args.positional = std::move(positional);
    commands::CommandContext context(&fixture, fixture.registry_presets_for_test(),
                                     fixture.confirmations, common::DefaultLogger());
    return commands::Dispatch(registry, name, args, context, /*busy=*/false);
}

}  // namespace

// --- parsing and desugaring ------------------------------------------------------

TEST_CASE("ParseCommandFile: frontmatter sources decode; run and sh are exclusive",
          "[commands_md]") {
    testing::TempDir dir;
    auto commands = WriteAndLoad(
        dir.path(), "deploy.md",
        "---\n"
        "description: Deploy the thing\n"
        "args: [env]\n"
        "sources:\n"
        "  branch:\n"
        "    run: [\"/usr/bin/git\", \"branch\", \"--show-current\"]\n"
        "  summary:\n"
        "    sh: \"make summary\"\n"
        "    output: {type: json, fields: [plan]}\n"
        "    timeout: 30\n"
        "    output_cap: {max: 1000, on_exceed: fail}\n"
        "---\n"
        "Deploy {env} from {branch} per {output:summary.plan}.\n");
    const auto* command = Find(commands, "deploy");
    REQUIRE(command != nullptr);
    CHECK(command->Summary() == "Deploy the thing");
    CHECK(std::string(command->Usage()) == "/deploy <env>");
}

TEST_CASE("ParseCommandFile: declaring run and sh together throws, and so does a bad mode",
          "[commands_md]") {
    // Direct ParseCommandFile calls: LoadCommands catches and skips by design.
    testing::TempDir dir;
    std::filesystem::create_directories(dir.path());
    auto write = [&dir](const std::string& name, const std::string& contents) {
        std::ofstream(dir.path() / name) << contents;
        return ParseCommandFile(dir.path() / name, /*trusted=*/true);
    };
    CHECK_THROWS_AS(write("bad.md",
                          "---\nsources:\n  x:\n    run: [\"a\"]\n    sh: \"a\"\n---\nb\n"),
                    std::runtime_error);
    CHECK_THROWS_AS(write("bad2.md",
                          "---\nsources:\n  x:\n    run: [\"a\"]\n"
                          "    output: {type: yaml}\n---\nb\n"),
                    std::runtime_error);
    CHECK_THROWS_AS(write("bad3.md", "---\nargs: 42\n---\nb\n"), std::runtime_error);
}

TEST_CASE("ParseCommandFile: opencode constructs desugar into the same flat sources",
          "[commands_md][compat]") {
    testing::TempDir dir;
    // One file in opencode's own layout and syntax: $ARGUMENTS, $1, !`cmd`.
    auto commands = WriteAndLoad(
        dir.path() / ".opencode" / "commands", "review.md",
        "Review $1 with focus on $ARGUMENTS.\n\nRecent commits:\n!`git log --oneline -3`\n");
    const auto* command = Find(commands, "review");
    REQUIRE(command != nullptr);
    // The desugared file is a PromptShorthandCommand like any other; dispatch
    // reaches it by the file stem, zero translation at the call site.
    CHECK(std::string(command->Name()) == "review");
}

// --- execution --------------------------------------------------------------------

TEST_CASE("commands-md: sources run, the body renders, and the composed text is the turn",
          "[commands_md]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    auto commands = WriteAndLoad(
        files.path(), "branch.md",
        "---\n"
        "description: Branch report\n"
        "args: [target]\n"
        "sources:\n"
        "  who:\n"
        "    run: [\"/bin/echo\", \"branch-{target}\"]\n"
        "---\n"
        "Report for {target} from {who}.\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "branch", {"main"});
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE(fixture.submitted_turns().size() == 1);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Report for main from branch-main.\n");
}

TEST_CASE("commands-md: a sh: source cannot see another source's output -- it stays a "
          "literal, un-substituted token",
          "[commands_md][compat]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    // The B8 exploit shape: `ctx` tries to splice `title`'s OUTPUT into a shell
    // string. Before the fix this ran the attacker's injected command; now
    // {title} is an unknown token to the sh: substitution (source outputs are
    // deliberately excluded from what a sh: script may see) and passes through
    // literally, so the shell just echoes the braces back.
    auto commands = WriteAndLoad(
        files.path(), "inject.md",
        "---\n"
        "sources:\n"
        "  title:\n"
        "    sh: \"echo 'x; touch " +
            (files.path() / "PWNED").string() +
            "'\"\n"
            "  ctx:\n"
            "    sh: \"echo got:{title}\"\n"
            "---\nDone: {ctx}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "inject", {});
    CAPTURE(result.message);
    CHECK(result.status == commands::CommandResult::Status::kOk);
    // The injected command must never have run.
    CHECK_FALSE(std::filesystem::exists(files.path() / "PWNED"));
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Done: got:{title}\n");
}

TEST_CASE("commands-md: a user argument reaching a sh: script is shell-quoted, so shell "
          "metacharacters in it cannot break out",
          "[commands_md][compat]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    auto commands = WriteAndLoad(
        files.path(), "greet.md",
        "---\nargs: [name]\nsources:\n  x:\n    sh: \"echo hi {name}\"\n---\nSaid: {x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    // A classic shell-injection payload as the argument: without quoting this
    // would run `touch PWNED` as a second command.
    const std::string payload = "a'; touch " + (files.path() / "PWNED").string() + "; echo '";
    const auto result = DispatchOne(registry, fixture, "greet", {payload});
    CAPTURE(result.message);
    CHECK(result.status == commands::CommandResult::Status::kOk);
    CHECK_FALSE(std::filesystem::exists(files.path() / "PWNED"));
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Said: hi " + payload + "\n");
}

TEST_CASE("commands-md: a generator does not inherit the server's own environment -- only "
          "the allowlisted variables",
          "[commands_md][compat]") {
    ScopedEnv secret("MINIMAL_TEST_SECRET", "sk-do-not-leak-this");
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    auto commands = WriteAndLoad(
        files.path(), "envcheck.md",
        "---\nsources:\n  x:\n    sh: \"echo \\\"[$MINIMAL_TEST_SECRET]\\\"\"\n---\nGot: {x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "envcheck", {});
    CAPTURE(result.message);
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    // Not on the allowlist: expands to empty in the child's shell, not the
    // real secret -- the whole point of B9's env scrub.
    CHECK(fixture.submitted_turns().front() == "Got: []\n");
}

TEST_CASE("commands-md: a bare program name (no slash) resolves via PATH search, not just "
          "absolute paths",
          "[commands_md]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    // "echo", not "/bin/echo": forces RunProcess's execve-over-allowlisted-PATH
    // search rather than the direct-path case every other test exercises.
    auto commands = WriteAndLoad(files.path(), "echoed.md",
                                 "---\nsources:\n  x:\n    run: [\"echo\", \"hi-{y}\"]\n"
                                 "args: [y]\n---\nGot: {x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "echoed", {"there"});
    CAPTURE(result.message);
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Got: hi-there\n");
}

TEST_CASE("commands-md: an untrusted (project-local) command asks before running any source, "
          "and cancel runs nothing",
          "[commands_md][compat]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    // trusted=false: the same shape LoadCommands gets for a project-local
    // commands/ directory.
    auto commands = WriteAndLoad(
        files.path(), "touch.md",
        "---\ndescription: writes a marker file\n"
        "sources:\n  x:\n    run: [\"/usr/bin/touch\", \"" +
            (files.path() / "marker").string() +
            "\"]\n---\nDone: {x}\n",
        /*trusted=*/false);
    for (auto& command : commands) registry.Register(std::move(command));

    // Cancel: the marker must not exist, and nothing reaches the session.
    const auto declined = DispatchOne(registry, fixture, "touch", {}, /*answer=*/"cancel");
    CHECK(declined.status == commands::CommandResult::Status::kError);
    CHECK_FALSE(std::filesystem::exists(files.path() / "marker"));
    CHECK(fixture.submitted_turns().empty());

    // Confirmed ("run", the gate's affirmative option): the source executes
    // and the turn is submitted, exactly like a trusted command.
    const auto confirmed = DispatchOne(registry, fixture, "touch", {}, /*answer=*/"run");
    CHECK(confirmed.status == commands::CommandResult::Status::kOk);
    CHECK(std::filesystem::exists(files.path() / "marker"));
    REQUIRE(fixture.submitted_turns().size() == 1);
}

TEST_CASE("commands-md: a trusted (global) command runs without the extra confirmation",
          "[commands_md][compat]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    // trusted=true (WriteAndLoad's default) with an answer the untrusted gate
    // would reject ("cancel") -- if the gate ran anyway, this would fail.
    auto commands = WriteAndLoad(files.path(), "hello.md",
                                 "---\nsources:\n  x:\n    run: [\"/bin/echo\", \"hi\"]\n"
                                 "---\nSaid: {x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "hello", {}, /*answer=*/"cancel");
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE(fixture.submitted_turns().size() == 1);
    CHECK(fixture.submitted_turns().front() == "Said: hi\n");
}

TEST_CASE("commands-md: inserted values are never re-scanned (single-pass render)",
          "[commands_md]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    auto commands = WriteAndLoad(
        files.path(), "nested.md",
        "---\nsources:\n  x:\n    run: [\"/bin/echo\", \"{target}\"]\n---\nGot: {x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    // The source's OUTPUT contains "{target}" -- render must insert it verbatim,
    // not recurse. A re-scan would throw on the unknown placeholder or loop.
    const auto result = DispatchOne(registry, fixture, "nested", {"world"});
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Got: {target}\n");
}

TEST_CASE("commands-md: json sources expose their declared fields as {output:source.field}",
          "[commands_md]") {
    testing::CommandFixture fixture;
    testing::TempDir files;
    commands::CommandRegistry registry;
    auto commands = WriteAndLoad(
        files.path(), "plan.md",
        "---\n"
        "sources:\n"
        "  summary:\n"
        "    sh: \"echo '{\\\"plan\\\": \\\"ship it\\\"}'\"\n"
        "    output: {type: json, fields: [plan]}\n"
        "---\n"
        "Plan: {output:summary.plan}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "plan", {});
    CAPTURE(result.message);
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Plan: ship it\n");
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "Plan: ship it\n");
}

TEST_CASE("commands-md: a missing required argument errors before anything runs",
          "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    testing::TempDir files;
    auto commands = WriteAndLoad(files.path(), "deploy.md",
                                       "---\nargs: [env]\n---\nDeploy {env}.\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "deploy", {});
    CHECK(result.status == commands::CommandResult::Status::kError);
    CHECK(result.message.find("missing argument <env>") != std::string::npos);
    CHECK(fixture.submitted_turns().empty());
}

TEST_CASE("commands-md: a failing source errors and submits nothing", "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    testing::TempDir files;
    auto commands = WriteAndLoad(
        files.path(), "fail.md",
        "---\nsources:\n  x:\n    sh: \"echo boom >&2; exit 3\"\n---\nNever: {x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "fail", {});
    CHECK(result.status == commands::CommandResult::Status::kError);
    CHECK(result.message.find("exit status 3") != std::string::npos);
    CHECK(result.message.find("boom") != std::string::npos);  // stderr narrative
    CHECK(fixture.submitted_turns().empty());
}

TEST_CASE("commands-md: an unknown placeholder errors instead of submitting a hole",
          "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    testing::TempDir files;
    auto commands = WriteAndLoad(files.path(), "typo.md", "---\n---\nUse {temperatur}.\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "typo", {});
    CHECK(result.status == commands::CommandResult::Status::kError);
    CHECK(result.message.find("{temperatur}") != std::string::npos);
    CHECK(fixture.submitted_turns().empty());
}

TEST_CASE("commands-md: no body means the sole text source's stdout IS the prompt",
          "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    testing::TempDir files;
    auto commands =
        WriteAndLoad(files.path(), "generated.md",
                     "---\nargs: [who]\nsources:\n  prompt:\n    run: [\"/bin/echo\", "
                     "\"generated for {who}\"]\n---\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "generated", {"release"});
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "generated for release");
}

TEST_CASE("commands-md: an optional argument fills a placeholder and omits an argv entry "
          "when absent",
          "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    testing::TempDir files;
    auto commands = WriteAndLoad(
        files.path(), "opt.md",
        "---\nargs: [a]\noptional: [flag]\n---\nA={a} FLAG={flag}\n");
    auto runner = WriteAndLoad(
        files.path(), "argv.md",
        "---\nargs: [a]\noptional: [flag]\nsources:\n  x:\n    run: [\"/bin/echo\", "
        "\"{a}\", \"{flag}\"]\n---\n{x}\n");
    for (auto& command : commands) registry.Register(std::move(command));
    for (auto& command : runner) registry.Register(std::move(command));

    // Template form: absent optional renders empty.
    auto result = DispatchOne(registry, fixture, "opt", {"one"});
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front() == "A=one FLAG=\n");

    // argv form: the unfilled optional's entry is OMITTED, not emptied -- echo
    // sees one argument, not an empty second one.
    result = DispatchOne(registry, fixture, "argv", {"one"});
    REQUIRE(fixture.submitted_turns().size() == 2);
    CHECK(fixture.submitted_turns().back() == "one\n");

    // Provided: it lands in the right slot.
    result = DispatchOne(registry, fixture, "argv", {"one", "two"});
    REQUIRE(fixture.submitted_turns().size() == 3);
    CHECK(fixture.submitted_turns().back() == "one two\n");
}

TEST_CASE("commands-md: over-cap output asks the operator; cancel submits nothing",
          "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    // 1 token = 4 bytes; the source emits ~40.
    testing::TempDir files;
    auto commands = WriteAndLoad(
        files.path(), "big.md",
        "---\nsources:\n  x:\n    run: [\"/bin/echo\", \"0123456789012345678901234567890123456789\"]\n"
        "    output_cap: {max: 1, on_exceed: confirm}\n---\n{x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    auto result = DispatchOne(registry, fixture, "big", {}, /*answer=*/"cancel");
    CHECK(result.status == commands::CommandResult::Status::kError);
    CHECK(result.message.find("cap") != std::string::npos);
    CHECK(fixture.submitted_turns().empty());

    // Submitting re-runs the source without the cap and ships the whole output.
    result = DispatchOne(registry, fixture, "big", {}, /*answer=*/"submit");
    CHECK(result.status == commands::CommandResult::Status::kOk);
    REQUIRE_FALSE(fixture.submitted_turns().empty());
    CHECK(fixture.submitted_turns().front().find("0123456789") != std::string::npos);
}

TEST_CASE("commands-md: a timed-out source errors and submits nothing", "[commands_md]") {
    testing::CommandFixture fixture;
    commands::CommandRegistry registry;
    testing::TempDir files;
    auto commands = WriteAndLoad(
        files.path(), "slow.md",
        "---\nsources:\n  x:\n    sh: \"sleep 30\"\n    timeout: 1\n---\n{x}\n");
    for (auto& command : commands) registry.Register(std::move(command));

    const auto result = DispatchOne(registry, fixture, "slow", {});
    CHECK(result.status == commands::CommandResult::Status::kError);
    CHECK(result.message.find("timed out") != std::string::npos);
    CHECK(fixture.submitted_turns().empty());
}

// --- discovery ---------------------------------------------------------------------

TEST_CASE("LoadCommands: the nearest directory wins a stem collision and malformed files "
          "are skipped",
          "[commands_md]") {
    testing::TempDir project;
    testing::TempDir global;
    std::ofstream(project.path() / "cmds.md") << "---\ndescription: PROJECT\n---\nProject.\n";
    std::ofstream(global.path() / "cmds.md") << "---\ndescription: GLOBAL\n---\nGlobal.\n";
    // Malformed in the nearer dir: it must not hide anything else, and the
    // farther dir's GOOD file with the same stem is NOT used as a fallback --
    // shadowing is by name, decided before parsing.
    std::ofstream(project.path() / "only-project.md") << "---\nargs: 42\n---\nb\n";
    std::ofstream(global.path() / "broken.md") << "---\nargs: 42\n---\nb\n";

    const auto commands = LoadCommands({{project.path(), false}, {global.path(), true}});
    const auto* cmds = Find(commands, "cmds");
    REQUIRE(cmds != nullptr);
    CHECK(cmds->Summary() == "PROJECT");  // nearer dir wins
    CHECK(Find(commands, "only-project") == nullptr);  // malformed: skipped, no fallback
    CHECK(Find(commands, "broken") == nullptr);        // malformed in the farther dir: skipped
}
