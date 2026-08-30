#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

#include "fakes.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

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

TEST_CASE("Bypass overrides an explicit deny", "[tool_executor][run_mode]") {
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, DenyAllPolicy(), {},
                                 PermissionRunMode::Bypass);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "bash", nlohmann::json::object(), ctx);

    CHECK_FALSE(block.is_error);
    CHECK(f.tool->calls.size() == 1);
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
