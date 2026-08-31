#include <catch2/catch_test_macros.hpp>

#include <sys/stat.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "fakes.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/tools/tool_executor.hpp"

using namespace minimal;
using namespace minimal::testing;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionRunMode;
using minimal::tools::ToolCategory;

namespace {

// A veto action that BLOCKS and records its payload, via a script writing to a
// known file. The dispatcher runs real /bin/sh, so the test doubles as an
// integration check of the whole chain (executor -> dispatcher -> shell).
struct BlockingHook {
    explicit BlockingHook(const std::filesystem::path& dir, const std::string& reason)
        : dispatcher(minimal::common::DefaultLogger()) {
        const auto script = dir / "block.sh";
        {
            std::ofstream out(script);
            out << "echo '" << reason << "'\nexit 2\n";
        }
        ::chmod(script.string().c_str(), 0755);

        minimal::config::HookActionConfig action;
        action.on = minimal::hooks::kPreToolUse;
        action.command = script.string();
        minimal::config::HooksConfig config;
        config.hooks.push_back(std::move(action));
        (void)dispatcher.Configure(config);
    }

    minimal::hooks::HookDispatcher dispatcher;
};

// An observe-only action recording that it ran, via a side-effect file.
struct RecordingHook {
    explicit RecordingHook(const std::filesystem::path& dir) : dispatcher(minimal::common::DefaultLogger()) {
        const auto script = dir / "record.sh";
        {
            std::ofstream out(script);
            out << "echo \"$1\" > /dev/null\ntouch '" << (dir / "prompted").string() << "'\nexit 0\n";
        }
        ::chmod(script.string().c_str(), 0755);

        minimal::config::HookActionConfig action;
        action.on = minimal::hooks::kPermissionPrompted;
        action.command = script.string();
        minimal::config::HooksConfig config;
        config.hooks.push_back(std::move(action));
        (void)dispatcher.Configure(config);
    }

    minimal::hooks::HookDispatcher dispatcher;
};

struct Fixture {
    tools::ToolRegistry registry;
    FakeTool* tool = nullptr;

    FakeTool& AddTool(std::string name, ToolCategory category) {
        auto owned = std::make_unique<FakeTool>(std::move(name), category, "ok");
        FakeTool* raw = owned.get();
        registry.Register(std::move(owned));
        tool = raw;
        return *raw;
    }

    tools::ToolContext Context() {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = std::filesystem::temp_directory_path();
        return ctx;
    }
};

std::string TextOf(const llm::ToolResultBlock& block) {
    if (block.content.empty()) return {};
    return std::get<llm::TextBlock>(block.content.front()).text;
}

}  // namespace

TEST_CASE("a pre_tool_use hook deny becomes an error result and never executes the tool",
          "[hooks][tool_executor]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_executor_test";
    std::filesystem::create_directories(dir);
    BlockingHook hook{dir, "guarded"};

    Fixture f;
    f.AddTool("read", ToolCategory::ReadOnly);  // policy allows it outright
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal, std::nullopt, std::nullopt, {},
                                 &hook.dispatcher);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "read", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK(TextOf(block).find("blocked by hook: guarded") != std::string::npos);
    CHECK(f.tool->calls.empty());   // the tool never ran
    CHECK_FALSE(checker.called);    // and no prompt was ever raised

    std::filesystem::remove_all(dir);
}

TEST_CASE("a passing hook leaves the normal path untouched", "[hooks][tool_executor]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_executor_test2";
    std::filesystem::create_directories(dir);

    minimal::hooks::HookDispatcher dispatcher{minimal::common::DefaultLogger()};
    minimal::config::HookActionConfig action;
    action.on = minimal::hooks::kPreToolUse;
    action.command = "exit 0";
    minimal::config::HooksConfig config;
    config.hooks.push_back(std::move(action));
    (void)dispatcher.Configure(config);

    Fixture f;
    f.AddTool("read", ToolCategory::ReadOnly);
    UnreachablePermissionChecker checker;

    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal, std::nullopt, std::nullopt, {},
                                 &dispatcher);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "read", nlohmann::json::object(), ctx);

    CHECK_FALSE(block.is_error);
    CHECK(f.tool->calls.size() == 1);

    std::filesystem::remove_all(dir);
}

TEST_CASE("AllowOnly short-circuits BEFORE the hook runs", "[hooks][tool_executor]") {
    // The invariant "hooks only narrow, and the substrate decides first": an
    // Ask-gated tool under AllowOnly resolves to AlwaysDeny and never reaches
    // the hook seam. Both hook points are wired with observable side effects;
    // a regression that moves the seam above the substrate would run one.
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_executor_test3";
    std::filesystem::create_directories(dir);

    RecordingHook hook{dir};  // observe-only, on permission_prompted: must NOT fire

    minimal::config::HookActionConfig pre_action;
    pre_action.on = minimal::hooks::kPreToolUse;
    pre_action.command = "touch '" + (dir / "pre_ran").string() + "'";
    minimal::config::HooksConfig pre_config;
    pre_config.hooks.push_back(std::move(pre_action));
    (void)hook.dispatcher.Configure(pre_config);

    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    UnreachablePermissionChecker checker;

    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {},
                                 PermissionRunMode::AllowOnly, std::nullopt, std::nullopt, {},
                                 &hook.dispatcher);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "write", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    CHECK(TextOf(block).find("permission denied") != std::string::npos);
    CHECK(f.tool->calls.empty());
    CHECK_FALSE(checker.called);
    CHECK(!std::filesystem::exists(dir / "pre_ran"));   // the veto point never ran
    hook.dispatcher.WaitForIdleForTesting();
    CHECK(!std::filesystem::exists(dir / "prompted"));  // the observe point never ran

    std::filesystem::remove_all(dir);
}

TEST_CASE("permission_prompted fires when an Ask reaches the checker", "[hooks][tool_executor]") {
    const auto dir = std::filesystem::temp_directory_path() / "minimal_hooks_executor_test4";
    std::filesystem::create_directories(dir);
    RecordingHook hook{dir};

    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    AlwaysDenyPermissionChecker checker;  // the human answers "deny"

    tools::PermissionPolicy policy;
    policy.file_write = PermissionMode::Ask;

    tools::ToolExecutor executor(f.registry, checker, policy, {}, PermissionRunMode::Normal,
                                 std::nullopt, std::nullopt, {}, &hook.dispatcher);
    auto ctx = f.Context();
    const auto block = executor.Run("tu1", "write", nlohmann::json::object(), ctx);

    CHECK(block.is_error);
    hook.dispatcher.WaitForIdleForTesting();
    CHECK(std::filesystem::exists(dir / "prompted"));

    std::filesystem::remove_all(dir);
}
