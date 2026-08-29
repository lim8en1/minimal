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

    FakeTool& AddTool(std::string name, ToolCategory category) {
        auto owned = std::make_unique<FakeTool>(std::move(name), category);
        FakeTool* raw = owned.get();
        registry.Register(std::move(owned));
        tool = raw;
        return *raw;
    }

    tools::ToolContext Context(std::filesystem::path root = std::filesystem::temp_directory_path()) {
        tools::ToolContext ctx;
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
