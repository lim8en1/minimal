#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <memory>

#include "fakes.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::testing;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionRunMode;
using minimal::tools::ToolCategory;

namespace {

struct Fixture {
    tools::ToolRegistry registry;
    FakeTool* tool = nullptr;

    FakeTool& AddTool(std::string name, ToolCategory category, std::string result_text = "ok") {
        auto owned = std::make_unique<FakeTool>(std::move(name), category, std::move(result_text));
        FakeTool* raw = owned.get();
        registry.Register(std::move(owned));
        tool = raw;
        return *raw;
    }

    tools::ToolContext Context(std::filesystem::path root = std::filesystem::temp_directory_path()) {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = std::move(root);
        return ctx;
    }
};

std::string TextOf(const llm::ToolResultBlock& block) {
    if (block.content.empty()) return {};
    return std::get<llm::TextBlock>(block.content.front()).text;
}

}  // namespace

TEST_CASE("mcp_server_of extracts the server segment", "[tool_executor][mcp]") {
    CHECK(tools::mcp_server_of("mcp__search__query") == "search");
    CHECK(tools::mcp_server_of("bash").empty());
    CHECK(tools::mcp_server_of("mcp__malformed").empty());
}

TEST_CASE("AlwaysAllow executes without consulting the checker", "[tool_executor]") {
    Fixture f;
    f.AddTool("read", ToolCategory::ReadOnly);
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "read", nlohmann::json::object(), ctx);

    CHECK_FALSE(block.is_error);
    CHECK(block.tool_use_id == "tu1");
    CHECK(f.tool->calls.size() == 1);
    CHECK_FALSE(checker.called);
}

TEST_CASE("a denied tool never executes", "[tool_executor]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    AlwaysDenyPermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "write", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK(f.tool->calls.empty());          // the point: gating happens before execution
    CHECK(checker.seen.size() == 1);
}

namespace {

class FixedResponseChecker : public tools::PermissionChecker {
public:
    explicit FixedResponseChecker(tools::PermissionResponse response)
        : response_(std::move(response)) {}
    tools::PermissionResponse check(const tools::PermissionRequest&) override { return response_; }

private:
    tools::PermissionResponse response_;
};

}  // namespace

TEST_CASE("a deny with a message carries the message into the denial result",
          "[tool_executor]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    FixedResponseChecker checker(
        {tools::PermissionDecision::Deny, {}, "use the edit tool instead", false});
    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto prepared = executor.Prepare("tu1", "write", nlohmann::json::object(), ctx);

    REQUIRE(prepared.denial.has_value());
    CHECK(prepared.denial->is_error);
    CHECK(TextOf(*prepared.denial).find("permission denied by user") != std::string::npos);
    CHECK(TextOf(*prepared.denial).find("use the edit tool instead") != std::string::npos);
    CHECK_FALSE(prepared.stop_turn);
}

TEST_CASE("deny and stop flags the prepared invocation", "[tool_executor]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    FixedResponseChecker checker({tools::PermissionDecision::Deny, {}, {}, true});
    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto prepared = executor.Prepare("tu1", "write", nlohmann::json::object(), ctx);

    REQUIRE(prepared.denial.has_value());
    CHECK(prepared.stop_turn);
    CHECK(TextOf(*prepared.denial).find("stopped the turn") != std::string::npos);
}

TEST_CASE("an allow ignores a stray deny message and stop", "[tool_executor]") {
    // Both fields refine a DENY; a client attaching them to an allow must not
    // turn it into a stop or leak text into a successful result.
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    FixedResponseChecker checker({tools::PermissionDecision::Allow, {}, "ignored", true});
    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto prepared = executor.Prepare("tu1", "write", nlohmann::json::object(), ctx);

    CHECK_FALSE(prepared.denial.has_value());
    CHECK_FALSE(prepared.stop_turn);
}

TEST_CASE("AlwaysDeny short-circuits before reaching the checker", "[tool_executor]") {
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, DenyAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "bash", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK_FALSE(checker.called);           // no pointless round-trip to ask about a certainty
    CHECK(f.tool->calls.empty());
}

TEST_CASE("AllowOnly run mode auto-denies rather than blocking", "[tool_executor][run_mode]") {
    // The whole reason AllowOnly exists: a headless run must not block forever on
    // a human who was never attached.
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::AllowOnly);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "bash", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK_FALSE(checker.called);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("Bypass preserves an explicit deny", "[tool_executor][run_mode]") {
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, DenyAllPolicy(), {},
                                 PermissionRunMode::Bypass);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "bash", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK_FALSE(checker.called);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("Bypass auto-approves an Ask without prompting", "[tool_executor][run_mode]") {
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Bypass);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "bash", nlohmann::json::object(), ctx);

    CHECK_FALSE(block.is_error);
    CHECK_FALSE(checker.called);
    CHECK(f.tool->calls.size() == 1);
}

TEST_CASE("Bypass cannot write past a file_write deny, but still approves Ask tools",
          "[tool_executor][run_mode]") {
    // The plan-preset shape: file_write denied, execute left at Ask.
    Fixture f;
    auto& write = f.AddTool("write", ToolCategory::FileWrite);
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::AlwaysDeny;
    policy.execute = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Bypass);
    auto ctx = f.Context();

    CHECK(executor.Run("t1", "write", nlohmann::json::object(), ctx).is_error);
    CHECK(write.calls.empty());
    CHECK_FALSE(executor.Run("t2", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK(bash.calls.size() == 1);
    CHECK_FALSE(checker.called);
}

TEST_CASE("Bypass cannot read past a deny pattern", "[tool_executor][run_mode][patterns]") {
    testing::TempDir dir;
    std::filesystem::create_directories(dir.path() / "root");
    std::ofstream(dir.path() / "root" / ".env") << "SECRET=1";
    std::ofstream(dir.path() / "root" / "main.cpp") << "int main(){}";

    Fixture f;
    auto& read = f.AddTool("read", ToolCategory::ReadOnly);
    read.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.pattern_overrides["read"].push_back({"**/.env", PermissionMode::AlwaysDeny});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Bypass);
    auto ctx = f.Context(dir.path() / "root");

    CHECK(executor.Run("t1", "read", {{"path", ".env"}}, ctx).is_error);
    CHECK_FALSE(executor.Run("t2", "read", {{"path", "main.cpp"}}, ctx).is_error);
}

TEST_CASE("Bypass cannot loosen a parent ceiling's deny", "[tool_executor][run_mode][ceiling]") {
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy ceiling = AllowAllPolicy();
    ceiling.execute = PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Bypass, ceiling);
    auto ctx = f.Context();

    CHECK(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("out-of-workdir folds in and cannot loosen the tool's own mode",
          "[tool_executor][out_of_workdir]") {
    // The regression this guards: if out_of_workdir REPLACED the category rather
    // than folding in, then file_write=deny + out_of_workdir=allow would permit
    // writing outside the project while denying writes inside it -- inverted.
    Fixture f;
    auto& tool = f.AddTool("write", ToolCategory::FileWrite);
    tool.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::AlwaysDeny;
    policy.out_of_workdir = PermissionMode::AlwaysAllow;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    const auto outside = executor.Run("tu1", "write", {{"path", "/etc/hosts"}}, ctx);
    CHECK(outside.is_error);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("out-of-workdir tightens an otherwise-allowed tool", "[tool_executor][out_of_workdir]") {
    Fixture f;
    auto& tool = f.AddTool("read", ToolCategory::ReadOnly);
    tool.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.read_only = PermissionMode::AlwaysAllow;
    policy.out_of_workdir = PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    const auto outside = executor.Run("tu1", "read", {{"path", "/etc/hosts"}}, ctx);
    CHECK(outside.is_error);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("network folds in and cannot loosen the tool's own mode",
          "[tool_executor][network]") {
    // Same class of regression as out-of-workdir: if network REPLACED the
    // category rather than folding in, then execute=deny + network=allow
    // would permit a network-capable tool while denying the category it
    // belongs to -- inverted.
    Fixture f;
    auto& tool = f.AddTool("web_fetch", ToolCategory::Execute);
    tool.set_network_access_required(true);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::AlwaysDeny;
    policy.network = PermissionMode::AlwaysAllow;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    const auto result = executor.Run("tu1", "web_fetch", nlohmann::json::object(), ctx);
    CHECK(result.is_error);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("network tightens an otherwise-allowed tool", "[tool_executor][network]") {
    Fixture f;
    auto& tool = f.AddTool("web_fetch", ToolCategory::Execute);
    tool.set_network_access_required(true);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::AlwaysAllow;
    policy.network = PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    const auto result = executor.Run("tu1", "web_fetch", nlohmann::json::object(), ctx);
    CHECK(result.is_error);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("network is never consulted for a tool that does not declare "
         "network_access_required",
         "[tool_executor][network]") {
    // A denied network dimension must not leak into unrelated tools -- the
    // fold only applies to tools that opt in.
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);   // network_access_required() defaults false
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::AlwaysAllow;
    policy.network = PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    const auto result = executor.Run("tu1", "bash", nlohmann::json::object(), ctx);
    CHECK_FALSE(result.is_error);
    CHECK(f.tool->calls.size() == 1);
}

// --- execute-time confinement re-check (TOCTOU) -------------------------------
//
// The Prepare-time classification is not a lock: between the two phases -- an
// Ask can sit on a human for minutes -- a parallel call can rename a symlink
// onto the path. Execute re-runs confine() and refuses an inside->outside
// divergence.

TEST_CASE("a path that escapes the workspace between Prepare and Execute is refused",
          "[tool_executor][toctou]") {
    testing::TempDir arena;
    std::filesystem::create_directories(arena.path() / "sub");
    std::filesystem::create_directory_symlink("sub", arena.path() / "link");

    Fixture f;
    auto& tool = f.AddTool("write", ToolCategory::FileWrite);
    tool.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context(arena.path());

    auto prepared = executor.Prepare("tu1", "write", {{"path", "link"}}, ctx);
    REQUIRE_FALSE(prepared.denial);
    REQUIRE(prepared.path_confinement_checked);
    REQUIRE(prepared.was_inside_project_root);

    // The race: repoint the symlink outside the root between the phases.
    std::filesystem::remove(arena.path() / "link");
    std::filesystem::create_directory_symlink("..", arena.path() / "link");

    const auto block = executor.Execute(prepared, ctx);
    CHECK(block.is_error);
    CHECK(TextOf(block).find("escaped the workspace") != std::string::npos);
    CHECK(tool.calls.empty());
}

TEST_CASE("Execute does not second-guess a Prepare-time outside classification",
          "[tool_executor][toctou]") {
    // Only a DIVERGENCE is refused. The out-of-workdir fold already priced an
    // outside path in at Prepare (possibly allowing it, when the operator set
    // out_of_workdir=allow); re-denying it at execute time would override that.
    Fixture f;
    auto& tool = f.AddTool("write", ToolCategory::FileWrite);
    tool.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();

    auto prepared = executor.Prepare("tu1", "write", {{"path", "/etc/hosts"}}, ctx);
    REQUIRE_FALSE(prepared.denial);
    REQUIRE(prepared.path_confinement_checked);
    REQUIRE_FALSE(prepared.was_inside_project_root);

    const auto block = executor.Execute(prepared, ctx);
    CHECK_FALSE(block.is_error);
    CHECK(tool.calls.size() == 1);
}

TEST_CASE("a path unresolvable at execute time fails closed against a recorded inside",
          "[tool_executor][toctou]") {
    testing::TempDir arena;
    std::filesystem::create_directories(arena.path() / "sub");

    Fixture f;
    auto& tool = f.AddTool("write", ToolCategory::FileWrite);
    tool.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context(arena.path());

    auto prepared = executor.Prepare("tu1", "write", {{"path", "sub"}}, ctx);
    REQUIRE(prepared.was_inside_project_root);

    // The swap this time leaves the path impossible to place at all: a
    // self-referential symlink loop. Unresolvable counts as outside.
    std::filesystem::remove(arena.path() / "sub");
    std::filesystem::create_directory_symlink("sub", arena.path() / "sub");

    const auto block = executor.Execute(prepared, ctx);
    CHECK(block.is_error);
    CHECK(tool.calls.empty());
}

TEST_CASE("MCP tools resolve per-server, not per-tool", "[tool_executor][mcp]") {
    Fixture f;
    f.AddTool("mcp__search__query", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::McpPermissionPolicy mcp;
    mcp.default_mode = PermissionMode::AlwaysDeny;
    mcp.per_server["search"] = PermissionMode::AlwaysAllow;

    // Execute is Ask in the built-in policy; the MCP chain must win for an
    // mcp__-prefixed name rather than falling through to it.
    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, mcp, PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "mcp__search__query", nlohmann::json::object(), ctx);

    CHECK_FALSE(block.is_error);
    CHECK_FALSE(checker.called);
    CHECK(f.tool->calls.size() == 1);
}

// --- consent scoping ---------------------------------------------------------
//
// Session grants exist to stop an `Ask` policy from asking about the same binary
// forty times. They are not a security boundary: a grant is blanket permission
// for its key, and for bash a disclosure is only ever a lower bound.

namespace {

tools::PermissionDisclosure Disclosing(std::vector<std::string> keys, bool complete = true) {
    tools::PermissionDisclosure disclosure;
    disclosure.summary = "summary";
    disclosure.complete = complete;
    for (auto& key : keys) disclosure.grants.push_back(tools::GrantOffer{key, key + " *"});
    return disclosure;
}

tools::PermissionPolicy AskExecutePolicy() {
    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;
    return policy;
}

}  // namespace

TEST_CASE("a granted key silences later prompts for the same invocation",
          "[tool_executor][grants]") {
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "git status"}}, ctx).is_error);
    CHECK_FALSE(executor.Run("tu2", "bash", {{"command", "git log"}}, ctx).is_error);

    CHECK(checker.seen.size() == 1);  // the second invocation never asked
    CHECK(f.tool->calls.size() == 2);
}

TEST_CASE("every disclosed key must be granted, not just one",
          "[tool_executor][grants]") {
    // The invariant that stops `git status; rm -rf /` from riding in on a grant
    // for git: the check is a conjunction over everything disclosed.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "git status"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 1);

    // Now an invocation that also reaches rm. git alone cannot cover it.
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git", "bash:/bin/rm"}));
    CHECK_FALSE(executor.Run("tu2", "bash", {{"command", "git status; rm -rf /"}}, ctx).is_error);
    CHECK(checker.seen.size() == 2);
}

TEST_CASE("an incomplete disclosure always prompts, whatever is held",
          "[tool_executor][grants]") {
    // `eval "$X"` names nothing, so there is nothing to check. Vacuous truth is
    // the trap: "no keys to verify" must not evaluate to "all keys verified".
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);
    REQUIRE(executor.granted_count() == 1);

    tool.set_disclosure(Disclosing({}, /*complete=*/false));
    CHECK_FALSE(executor.Run("tu2", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK(checker.seen.size() == 2);

    // Same for a complete disclosure that simply named nothing.
    tool.set_disclosure(Disclosing({}, /*complete=*/true));
    CHECK_FALSE(executor.Run("tu3", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK(checker.seen.size() == 3);
}

TEST_CASE("a prompt offers only the keys the operator does not already hold",
          "[tool_executor][grants]") {
    // `cd x; ls a | wc -l`, then `cd x; ls a | grep something`: the second
    // prompt used to re-list cd and ls -- already granted, idempotent to
    // re-confirm, pure prompt-fatigue noise. The conjunction keeps running on
    // the unfiltered disclosure (the prompt still fires while grep is
    // missing); only what is SHOWN gets filtered down to the new keys.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(
        Disclosing({"bash:/usr/bin/cd", "bash:/bin/ls", "bash:/usr/bin/wc"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "cd x; ls a | wc -l"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 1);
    CHECK(checker.seen[0].disclosure.grants.size() == 3);  // nothing held yet
    REQUIRE(executor.granted_count() == 3);

    tool.set_disclosure(
        Disclosing({"bash:/usr/bin/cd", "bash:/bin/ls", "bash:/usr/bin/grep"}));
    CHECK_FALSE(
        executor.Run("tu2", "bash", {{"command", "cd x; ls a | grep something"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 2);
    REQUIRE(checker.seen[1].disclosure.grants.size() == 1);  // grep only
    CHECK(checker.seen[1].disclosure.grants[0].key == "bash:/usr/bin/grep");

    // Every key already held: no prompt at all -- the conjunction decides,
    // not the filtered offer list.
    tool.set_disclosure(Disclosing({"bash:/usr/bin/cd", "bash:/bin/ls"}));
    CHECK_FALSE(executor.Run("tu3", "bash", {{"command", "cd x; ls"}}, ctx).is_error);
    CHECK(checker.seen.size() == 2);
    CHECK(f.tool->calls.size() == 3);
}

TEST_CASE("two offers for the same binary collapse to one prompt row",
          "[tool_executor][grants]") {
    // Different words, same binary: `ls` and `/bin/ls` both resolve to
    // bash:/bin/ls. They mint the same key, so showing two "allow /bin/ls *"
    // rows for one command was noise (and two ticks for one grant).
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/bin/ls", "bash:/usr/bin/git", "bash:/bin/ls"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "ls; git status; /bin/ls"}}, ctx).is_error);

    REQUIRE(checker.seen.size() == 1);
    REQUIRE(checker.seen[0].disclosure.grants.size() == 2);
    CHECK(checker.seen[0].disclosure.grants[0].key == "bash:/bin/ls");   // first wins
    CHECK(checker.seen[0].disclosure.grants[1].key == "bash:/usr/bin/git");
    CHECK(executor.granted_count() == 2);
}

TEST_CASE("an incomplete disclosure with all nameable keys held prompts with no offers",
          "[tool_executor][grants]") {
    // `eval x; ls` with ls already granted: the held key cannot be re-offered,
    // and the unnameable part never could be. The prompt still fires (the
    // incomplete rule) -- with zero grant checkboxes, allow-once/deny only.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/bin/ls"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "ls"}}, ctx).is_error);
    REQUIRE(executor.granted_count() == 1);

    tool.set_disclosure(Disclosing({"bash:/bin/ls"}, /*complete=*/false));
    CHECK_FALSE(executor.Run("tu2", "bash", {{"command", "eval x; ls"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 2);
    CHECK(checker.seen[1].disclosure.grants.empty());
    CHECK(executor.granted_count() == 1);  // allow-once minted nothing new
}

TEST_CASE("a checker cannot grant a key it was never shown", "[tool_executor][grants]") {
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));
    // Claims a grant on rm, which this invocation never disclosed.
    GrantingPermissionChecker checker({"bash:/bin/rm"});

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);

    CHECK(executor.granted_count() == 0);
}

TEST_CASE("grants are inert under AllowOnly", "[tool_executor][grants][run_mode]") {
    // A headless run cannot prompt, so no grant can ever be created -- and none
    // may be seeded from config either. That is what keeps a consent affordance
    // from becoming a persisted allowlist.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::AllowOnly);
    auto ctx = f.Context();
    CHECK(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK_FALSE(checker.called);
    CHECK(executor.granted_count() == 0);
}

TEST_CASE("a tool with no output still yields a non-empty result block", "[tool_executor]") {
    // `touch x` succeeds silently. An empty text block is a hard 400 on the
    // Anthropic wire format, so this is fixed centrally rather than per tool.
    Fixture f;
    f.AddTool("silent", ToolCategory::ReadOnly, /*result_text=*/"");
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "silent", nlohmann::json::object(), ctx);

    CHECK_FALSE(block.is_error);
    REQUIRE_FALSE(block.content.empty());
    CHECK_FALSE(TextOf(block).empty());
}

TEST_CASE("an unknown tool is an error result, not a crash", "[tool_executor]") {
    Fixture f;
    UnreachablePermissionChecker checker;
    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "nope", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK(TextOf(block).find("unknown tool") != std::string::npos);
}

// --- subagent ceilings --------------------------------------------------------
//
// A child session may never be more permissive than its parent. The parent's
// policies arrive as ceilings folded in via most_restrictive() -- remove that
// fold and every test in this block fails.

TEST_CASE("a policy ceiling tightens an otherwise-allowed category",
          "[tool_executor][ceiling]") {
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    GrantingPermissionChecker checker;

    // Child preset allows execute outright; the parent's Ask policy is the ceiling.
    tools::PermissionPolicy ceiling = AllowAllPolicy();
    ceiling.execute = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal, ceiling);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);

    CHECK(checker.seen.size() == 1);  // the ceiling forced the prompt
}

TEST_CASE("a child's per-tool override cannot buy back what the ceiling forbids",
          "[tool_executor][ceiling]") {
    // The whole reason ceilings fold at resolve time instead of merging policies
    // up front: mode_for() knows the tool's category, a merged policy no longer
    // does. A child override of `bash: allow` against a parent whose execute
    // category is Ask must still prompt.
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    GrantingPermissionChecker checker;

    tools::PermissionPolicy child = AllowAllPolicy();
    child.per_tool_overrides["bash"] = PermissionMode::AlwaysAllow;

    tools::PermissionPolicy ceiling;  // defaults: file_write/execute/out_of_workdir = Ask
    ceiling.read_only = PermissionMode::AlwaysAllow;

    tools::ToolExecutor executor(f.registry, checker, child, {}, PermissionRunMode::Normal,
                                 ceiling);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);

    CHECK(checker.seen.size() == 1);
}

TEST_CASE("a ceiling can only tighten, never loosen", "[tool_executor][ceiling]") {
    // Invariant direction check: a permissive ceiling over a restrictive child
    // policy changes nothing. If the fold were a substitution this would run
    // unprompted.
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    GrantingPermissionChecker checker;

    tools::PermissionPolicy child;
    child.execute = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, child, {}, PermissionRunMode::Normal,
                                 AllowAllPolicy());
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);

    CHECK(checker.seen.size() == 1);
}

TEST_CASE("a ceiling's out_of_workdir dimension governs a child's outside-path call",
          "[tool_executor][ceiling][out_of_workdir]") {
    // out_of_workdir is a dimension FIELD, not a category, so the ceiling's
    // mode_for() never consulted it: without the explicit fold a child preset
    // with out_of_workdir=allow would exceed its parent on exactly that axis.
    Fixture f;
    auto& tool = f.AddTool("write", ToolCategory::FileWrite);
    tool.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy child = AllowAllPolicy();
    tools::PermissionPolicy ceiling = AllowAllPolicy();
    ceiling.out_of_workdir = PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(f.registry, checker, child, {}, PermissionRunMode::Normal,
                                 ceiling);
    auto ctx = f.Context();

    const auto outside = executor.Run("tu1", "write", {{"path", "/etc/hosts"}}, ctx);
    CHECK(outside.is_error);
    CHECK(f.tool->calls.empty());

    // Inside the root the dimension does not apply: the same call on a
    // workdir path still runs.
    const auto inside = executor.Run("tu2", "write", {{"path", "local.txt"}}, ctx);
    CHECK_FALSE(inside.is_error);
    CHECK(f.tool->calls.size() == 1);
}

TEST_CASE("a ceiling's network dimension governs a network-capable tool",
          "[tool_executor][ceiling][network]") {
    // Same shape as the out_of_workdir ceiling: network is a field, not a
    // category, so only the explicit fold keeps a child under its parent.
    Fixture f;
    auto& tool = f.AddTool("web_fetch", ToolCategory::Execute);
    tool.set_network_access_required(true);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy child = AllowAllPolicy();
    tools::PermissionPolicy ceiling = AllowAllPolicy();
    ceiling.network = PermissionMode::AlwaysDeny;

    tools::ToolExecutor executor(f.registry, checker, child, {}, PermissionRunMode::Normal,
                                 ceiling);
    auto ctx = f.Context();

    const auto result = executor.Run("tu1", "web_fetch", nlohmann::json::object(), ctx);
    CHECK(result.is_error);
    CHECK(f.tool->calls.empty());
}

TEST_CASE("an mcp ceiling governs mcp tools, not the policy ceiling",
          "[tool_executor][ceiling][mcp]") {
    Fixture f;
    f.AddTool("mcp__search__query", ToolCategory::Execute);
    GrantingPermissionChecker checker;

    tools::McpPermissionPolicy mcp;
    mcp.default_mode = PermissionMode::AlwaysAllow;  // the child would run it freely

    tools::McpPermissionPolicy mcp_ceiling;
    mcp_ceiling.default_mode = PermissionMode::Ask;  // the parent asked first

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), mcp,
                                 PermissionRunMode::Normal, AllowAllPolicy(), mcp_ceiling);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "mcp__search__query", nlohmann::json::object(), ctx).is_error);

    CHECK(checker.seen.size() == 1);
}

TEST_CASE("seeded grants silence prompts the parent already answered",
          "[tool_executor][grants]") {
    // Copy-at-spawn: re-prompting for every binary the parent's operator
    // approved would make subagents unusable.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));
    GrantingPermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AskExecutePolicy(), {},
                                 PermissionRunMode::Normal, std::nullopt, std::nullopt,
                                 {"bash:/usr/bin/git"});
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "git status"}}, ctx).is_error);

    CHECK(checker.seen.empty());  // never asked
    CHECK(executor.granted_keys() == std::vector<std::string>{"bash:/usr/bin/git"});
}

TEST_CASE("the executor tells the tool which invocation it is running",
          "[tool_executor]") {
    // A tool that ties follow-up work to its own call -- the subagent-spawn
    // completion is injected as a ToolResultBlock keyed to it -- reads
    // ToolContext::tool_use_id.
    struct IdCapturingTool : tools::Tool {
        const tools::ToolSpec& spec() const override { return spec_; }
        tools::ToolResult execute(const nlohmann::json&, tools::ToolContext& ctx) override {
            seen = ctx.tool_use_id;
            return {false, "ok"};
        }
        tools::ToolSpec spec_{"capture", ToolCategory::ReadOnly,
                              {"capture", "captures", nlohmann::json::object()}};
        std::string seen;
    };

    Fixture f;
    auto owned = std::make_unique<IdCapturingTool>();
    auto* tool = owned.get();
    f.registry.Register(std::move(owned));
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK_FALSE(executor.Run("tu-42", "capture", nlohmann::json::object(), ctx).is_error);
    CHECK(tool->seen == "tu-42");
}

TEST_CASE("an aliased tool still hits the ceiling's per-tool deny",
          "[tool_executor][ceiling][invariant]") {
    // THE PERMISSION-KEY RULE, second enforcement point. The ceiling is the
    // PARENT's policy, whose per_tool_overrides are keyed by permission key --
    // so matching the ceiling on the SURFACE name missed every aliased tool,
    // and the child ran exactly the tool the parent's operator had denied,
    // under its alias. The lookup must go through PermissionKeyFor() like the
    // base policy's.
    Fixture f;
    f.registry.SetAttribution("tools-x");
    f.AddTool("bash", ToolCategory::Execute);
    f.registry.ClearAttribution();
    f.registry.ApplyAliases({{"tools-x.bash", "ssh_bash"}});
    REQUIRE(f.registry.Find("ssh_bash") != nullptr);
    f.registry.SetPermissionKeys({{"ssh_bash", "bash"}});

    UnreachablePermissionChecker checker;

    tools::PermissionPolicy child = AllowAllPolicy();  // the child would run it freely
    tools::PermissionPolicy ceiling = AllowAllPolicy();
    ceiling.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;  // the parent denied it

    tools::ToolExecutor executor(f.registry, checker, child, {}, PermissionRunMode::Normal,
                                 ceiling);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "ssh_bash", nlohmann::json::object(), ctx);
    CHECK(block.is_error);           // denied outright: the ceiling's deny is the answer
    CHECK(f.tool->calls.empty());    // never executed
    CHECK_FALSE(checker.called);     // never even consulted
}

// --- a throwing tool must still answer its call ------------------------------------------

namespace {

// Throws from whichever plugin entry point the test selects. All three are
// reachable with input the MODEL wrote, and all three used to escape Run().
class ThrowingTool : public tools::Tool {
public:
    enum class Where { Execute, CategoryFor, Disclose };

    explicit ThrowingTool(Where where) : where_(where) {
        spec_.name = "boom";
        spec_.category = ToolCategory::Execute;
        spec_.definition.name = spec_.name;
        spec_.definition.input_schema = nlohmann::json::object();
    }

    const tools::ToolSpec& spec() const override { return spec_; }

    tools::ToolResult execute(const nlohmann::json&, tools::ToolContext&) override {
        if (where_ == Where::Execute) throw std::runtime_error("kaboom");
        return tools::ToolResult{false, "ok"};
    }

    std::optional<ToolCategory> category_for(const nlohmann::json&) const override {
        if (where_ == Where::CategoryFor) throw std::runtime_error("kaboom");
        return std::nullopt;
    }

    tools::PermissionDisclosure Disclose(const nlohmann::json&,
                                         const tools::ToolContext&) const override {
        if (where_ == Where::Disclose) throw std::runtime_error("kaboom");
        return {};
    }

private:
    tools::ToolSpec spec_;
    Where where_;
};

}  // namespace

TEST_CASE("a throwing tool is answered, not propagated", "[tool_executor][robustness]") {
    // HISTORY INTEGRITY, not politeness. The loop persists the assistant message
    // carrying the tool_use before running any tool and appends results after
    // all of them, so an escaping exception leaves an orphaned tool_use that
    // both wire formats reject -- one bad input poisons every later turn of the
    // session. Every entry point below Find() is plugin code reading model-written
    // JSON, so a throw is expected rather than exceptional.
    const auto where = GENERATE(ThrowingTool::Where::Execute, ThrowingTool::Where::CategoryFor,
                                ThrowingTool::Where::Disclose);

    tools::ToolRegistry registry;
    registry.Register(std::make_unique<ThrowingTool>(where));

    AlwaysAllowPermissionChecker checker;
    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;  // reaches category_for AND Disclose
    tools::ToolExecutor executor(registry, checker, policy, {}, PermissionRunMode::Normal);

    tools::ToolContext ctx{common::DefaultLogger()};
    ctx.session_id = "s1";
    ctx.project_root = std::filesystem::temp_directory_path();

    llm::ToolResultBlock block;
    REQUIRE_NOTHROW(block = executor.Run("tu-42", "boom", nlohmann::json::object(), ctx));

    // The call is ANSWERED: same id, marked failed, with text. An unanswered or
    // misidentified block is the orphan this exists to prevent.
    CHECK(block.tool_use_id == "tu-42");
    CHECK(block.is_error);
    CHECK_FALSE(block.content.empty());
    CHECK(TextOf(block).find("boom") != std::string::npos);
}

// --- pattern rules ---------------------------------------------------------------

TEST_CASE("a matching bash command pattern replaces the category default",
          "[tool_executor][patterns]") {
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;   // the unmatched case must reach it

    tools::PermissionPolicy policy;  // execute defaults to Ask
    policy.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    // "git status" matches "git *": allow, no prompt. The command flavor
    // crosses '/' -- a command line is not a path.
    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git status"}}, ctx).is_error);
    CHECK(checker.seen.empty());
    CHECK_FALSE(
        executor.Run("t2", "bash", {{"command", "git commit -m src/x.cpp"}}, ctx).is_error);
    CHECK(checker.seen.empty());

    // "rm -rf /" matches nothing: the Ask default stands, the checker is asked.
    CHECK(executor.Run("t3", "bash", {{"command", "rm -rf /"}}, ctx).is_error == false);
    CHECK(checker.seen.size() == 1);
}


TEST_CASE("matching patterns fold most-restrictively: allow + ask is ask",
          "[tool_executor][patterns][invariant]") {
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy policy;
    auto& rules = policy.pattern_overrides["bash"];
    rules.push_back({"git *", PermissionMode::AlwaysAllow});
    rules.push_back({"git push --force *", PermissionMode::Ask});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    // Only "git *" matches: allow, no prompt.
    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git status"}}, ctx).is_error);
    CHECK(checker.seen.empty());
    // Both match: the most restrictive (ask) wins -- the operator's tighter
    // rule is not collapsed away by the looser one.
    CHECK_FALSE(executor.Run("t2", "bash", {{"command", "git push --force origin"}}, ctx)
                    .is_error);
    REQUIRE(checker.seen.size() == 1);
}

TEST_CASE("an exact per_tool_overrides entry outranks the pattern layer",
          "[tool_executor][patterns]") {
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;
    policy.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;
    policy.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    const auto block = executor.Run("t1", "bash", {{"command", "git status"}}, ctx);
    CHECK(block.is_error);   // denied outright...
    CHECK_FALSE(checker.called);  // ...and never prompted: exact beats pattern
}

TEST_CASE("path patterns anchor at the canonical path and deny dotenv reads",
          "[tool_executor][patterns]") {
    testing::TempDir dir;
    std::filesystem::create_directories(dir.path() / "root" / "src");
    std::filesystem::create_directories(dir.path() / "root" / ".config");
    // Real files: the confinement canonicalization must resolve them.
    std::ofstream(dir.path() / "root" / ".env") << "SECRET=1";
    std::ofstream(dir.path() / "root" / ".config" / ".env.local") << "SECRET=1";
    std::ofstream(dir.path() / "root" / "src" / "main.cpp") << "int main(){}";

    Fixture f;
    auto& read = f.AddTool("read", ToolCategory::ReadOnly);
    read.set_path_key("path");
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;  // read_only defaults to Allow
    auto& rules = policy.pattern_overrides["read"];
    rules.push_back({"**/.env", PermissionMode::AlwaysDeny});
    rules.push_back({"**/.env.*", PermissionMode::AlwaysDeny});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context(dir.path() / "root");

    // Every depth matches "**/.env"; a suffix matches the ".env.*" rule.
    CHECK(executor.Run("t1", "read", {{"path", ".env"}}, ctx).is_error);
    CHECK(executor.Run("t2", "read", {{"path", ".config/.env.local"}}, ctx).is_error);
    CHECK(checker.called == false);   // denied before any prompt could exist
    // An ordinary source file matches nothing: the category default (allow).
    CHECK_FALSE(executor.Run("t3", "read", {{"path", "src/main.cpp"}}, ctx).is_error);
}

TEST_CASE("external_directory patterns replace the scalar for matching paths",
          "[tool_executor][patterns]") {
    testing::TempDir dir;
    std::filesystem::create_directories(dir.path() / "root");
    std::filesystem::create_directories(dir.path() / "builds" / "a");
    std::ofstream(dir.path() / "builds" / "a" / "out.txt") << "x";
    std::ofstream(dir.path() / "builds" / "other.txt") << "x";
    // macOS resolves /var -> /private/var: patterns and inputs must both be
    // built from the CANONICAL base or the glob comparison silently misses.
    const auto base = std::filesystem::canonical(dir.path());
    std::filesystem::create_directories(base / "root");

    Fixture f;
    auto& read = f.AddTool("read", ToolCategory::ReadOnly);
    read.set_path_key("path");
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy policy;
    auto& patterns = policy.out_of_workdir_patterns;
    patterns.push_back({(base / "builds" / "a" / "**").string(), PermissionMode::AlwaysAllow});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context(base / "root");

    // Under the allowed subtree: the out_of_workdir Ask is REPLACED by allow.
    CHECK_FALSE(
        executor.Run("t1", "read", {{"path", (base / "builds" / "a" / "out.txt").string()}},
                     ctx)
            .is_error);
    CHECK(checker.seen.empty());
    // Outside the subtree but still outside the root: the scalar Ask stands.
    CHECK_FALSE(
        executor.Run("t2", "read", {{"path", (base / "builds" / "other.txt").string()}},
                     ctx)
            .is_error);
    CHECK(checker.seen.size() == 1);
}

TEST_CASE("the ceiling's patterns tighten a child's pattern-allow",
          "[tool_executor][patterns][invariant]") {
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy child_policy;  // the child's own allow
    child_policy.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});
    tools::PermissionPolicy ceiling;       // the parent's ask for the same commands
    ceiling.execute = PermissionMode::Ask;
    ceiling.pattern_overrides["bash"].push_back({"git *", PermissionMode::Ask});

    tools::ToolExecutor executor(f.registry, checker, child_policy, {},
                                 PermissionRunMode::Normal, ceiling);
    auto ctx = f.Context();

    // Child pattern says allow; ceiling pattern says ask; the fold keeps ask.
    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git status"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 1);
}

TEST_CASE("the ceiling's patterns match even when the child's own policy has none",
          "[tool_executor][patterns][invariant]") {
    // The regression: the pattern subject was computed only when the CHILD's
    // policy carried pattern rules, so a pattern-free child preset silenced
    // the parent's {"git *": "ask"} ceiling entirely -- exactly the common
    // configuration, since most child presets declare no patterns at all.
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy child_policy = AllowAllPolicy();  // no patterns at all
    // The ceiling's CATEGORY is permissive too, so the ceiling's PATTERN is
    // the only thing that can force the prompt below.
    tools::PermissionPolicy ceiling = AllowAllPolicy();
    ceiling.pattern_overrides["bash"].push_back({"git *", PermissionMode::Ask});

    tools::ToolExecutor executor(f.registry, checker, child_policy, {},
                                 PermissionRunMode::Normal, ceiling);
    auto ctx = f.Context();

    // The ceiling's pattern alone must force the prompt on a matching command...
    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git status"}}, ctx).is_error);
    REQUIRE(checker.seen.size() == 1);
    // ...and a non-matching command stays allowed on both sides.
    CHECK_FALSE(executor.Run("t2", "bash", {{"command", "ls -la"}}, ctx).is_error);
    CHECK(checker.seen.size() == 1);
}

// --- staged pattern rules (bash: per-stage matching) -------------------------------

TEST_CASE("an allow pattern requires every stage to match one (pipeline)",
          "[tool_executor][patterns][stages]") {
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.pattern_overrides["bash"].push_back({"git status", PermissionMode::AlwaysAllow});
    policy.pattern_overrides["bash"].push_back({"wc *", PermissionMode::AlwaysAllow});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    // Both stages covered: runs unprompted.
    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git status | wc -l"},
                                            {"stages", {"git status", "wc -l"}}},
                             ctx).is_error);
    CHECK(checker.seen.empty());
    // One stage uncovered: the Ask default stands.
    CHECK_FALSE(executor.Run("t2", "bash", {{"command", "git status | tee out"},
                                            {"stages", {"git status", "tee out"}}},
                             ctx).is_error);
    CHECK(checker.seen.size() == 1);
}

TEST_CASE("an allow pattern never matches through a whole-line prefix",
          "[tool_executor][patterns][stages][invariant]") {
    // The old shape: the subject was the raw line, so "git diff *" matched
    // `git diff; make install` -- the stage conjunction closes exactly that.
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.pattern_overrides["bash"].push_back({"git diff *", PermissionMode::AlwaysAllow});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git diff; make install"},
                                            {"stages", {"git diff", "make install"}}},
                             ctx).is_error);
    REQUIRE(checker.seen.size() == 1);  // prompted: `make install` matched nothing
}

TEST_CASE("ask and deny rules match any stage, not just the whole line",
          "[tool_executor][patterns][stages]") {
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    UnreachablePermissionChecker deny_checker;

    tools::PermissionPolicy deny;
    deny.execute = PermissionMode::AlwaysAllow;  // only the pattern can deny
    deny.pattern_overrides["bash"].push_back({"rm *", PermissionMode::AlwaysDeny});
    tools::ToolExecutor deny_executor(f.registry, deny_checker, deny, {},
                                      PermissionRunMode::Normal);
    auto ctx = f.Context();
    // The deny rule matches a stage hiding mid-pipeline: outright denial.
    CHECK(deny_executor.Run("t1", "bash", {{"command", "ls | rm -rf /"},
                                           {"stages", {"ls", "rm -rf /"}}},
                           ctx).is_error);
    CHECK_FALSE(deny_checker.called);  // denied before any prompt could exist

    // Ask: a stage-level ask rule forces the prompt despite a whole-line allow.
    AlwaysAllowPermissionChecker ask_checker;
    tools::PermissionPolicy ask;
    ask.execute = PermissionMode::AlwaysAllow;
    ask.pattern_overrides["bash"].push_back({"git push --force *", PermissionMode::Ask});
    tools::ToolExecutor ask_executor(f.registry, ask_checker, ask, {},
                                     PermissionRunMode::Normal);
    CHECK_FALSE(ask_executor.Run("t2", "bash",
                                 {{"command", "git status && git push --force origin"},
                                  {"stages", {"git status", "git push --force origin"}}},
                                 ctx).is_error);
    REQUIRE(ask_checker.seen.size() == 1);
}

TEST_CASE("staged allow does not override a matching ask or deny rule",
          "[tool_executor][patterns][stages][invariant]") {
    // Allow and ask both match the same stage: most_restrictive keeps ask.
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    AlwaysAllowPermissionChecker checker;

    tools::PermissionPolicy policy;
    auto& rules = policy.pattern_overrides["bash"];
    rules.push_back({"git *", PermissionMode::AlwaysAllow});
    rules.push_back({"git push --force *", PermissionMode::Ask});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "git status"},
                                            {"stages", {"git status"}}}, ctx).is_error);
    CHECK(checker.seen.empty());
    CHECK_FALSE(executor.Run("t2", "bash", {{"command", "git push --force origin"},
                                            {"stages", {"git push --force origin"}}},
                             ctx).is_error);
    CHECK(checker.seen.size() == 1);
}

TEST_CASE("a tool without stages keeps whole-line pattern semantics",
          "[tool_executor][patterns][stages]") {
    // The default PatternStages returns nullopt: the path-tool tests above and
    // any stage-free bash look-alike must be unchanged.
    Fixture f;
    auto& bash = f.AddTool("bash", ToolCategory::Execute);
    bash.set_command_subject(true);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;  // the allow pattern below must NOT apply
    policy.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});
    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal);
    auto ctx = f.Context();

    // `rm -rf /` matches nothing: plain Ask. Whole-line "git *"-shape behavior
    // for ask/deny is covered by the pre-existing cases above; here the point
    // is that stage-free matching still resolves and never allows silently.
    CHECK_FALSE(executor.Run("t1", "bash", {{"command", "rm -rf /"}}, ctx).is_error);
    REQUIRE(checker.called);
}
