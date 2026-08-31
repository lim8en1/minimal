#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "deadline.hpp"
#include "fakes.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;
using namespace minimal::testing;
using minimal::tools::PermissionDecision;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionResponse;
using minimal::tools::PermissionRunMode;
using minimal::tools::ToolCategory;

namespace {

struct ScriptedAdvisor : tools::PermissionAdvisor {
    // nullopt = abstain. When answering, grants every key the disclosure offered.
    std::optional<PermissionResponse> next;
    bool throw_std = false;
    bool throw_unknown = false;
    std::vector<tools::PermissionRequest> seen;

    std::optional<PermissionResponse> Advise(const tools::PermissionRequest& request,
                                             const tools::AdvisorContext&) override {
        seen.push_back(request);
        if (throw_std) throw std::runtime_error("advisor exploded");
        if (throw_unknown) throw 42;
        return next;
    }
};

tools::PermissionDisclosure Disclosing(std::vector<std::string> keys, bool complete = true) {
    tools::PermissionDisclosure d;
    d.complete = complete;
    for (auto& key : keys) d.grants.push_back({std::move(key), key + " *"});
    return d;
}

tools::PermissionPolicy AskExecutePolicy() {
    tools::PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;
    return policy;
}

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

    tools::ToolContext Context() {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = std::filesystem::temp_directory_path();
        return ctx;
    }
};

tools::ToolExecutor MakeExecutor(tools::ToolRegistry& registry, tools::PermissionChecker& checker,
                                 PermissionRunMode run_mode = PermissionRunMode::Normal) {
    return tools::ToolExecutor(registry, checker, AskExecutePolicy(), {}, run_mode);
}

}  // namespace

TEST_CASE("an advisor's Allow runs the tool without consulting the inner checker",
          "[permission_advisor]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    auto advisor = std::make_shared<ScriptedAdvisor>();
    advisor->next = PermissionResponse{PermissionDecision::Allow, {}};
    auto inner = std::make_unique<UnreachablePermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("tu1", "write", nlohmann::json::object(), ctx).is_error);
    CHECK(f.tool->calls.size() == 1);
    CHECK_FALSE(inner_ptr->called);
    REQUIRE(advisor->seen.size() == 1);
    CHECK(advisor->seen.front().tool_name == "write");
}

TEST_CASE("an advisor's Deny produces the same error result a human deny does",
          "[permission_advisor]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    auto advisor = std::make_shared<ScriptedAdvisor>();
    advisor->next = PermissionResponse{PermissionDecision::Deny, {}};
    auto inner = std::make_unique<UnreachablePermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    CHECK(executor.Run("tu1", "write", nlohmann::json::object(), ctx).is_error);
    CHECK(f.tool->calls.empty());
    CHECK_FALSE(inner_ptr->called);
}

TEST_CASE("an abstaining advisor falls through to the inner checker", "[permission_advisor]") {
    // The invariant that makes an advisor safe: abstain means the human, never
    // a silent denial. Removing the fall-through (abstain -> Deny) fails this.
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    auto advisor = std::make_shared<ScriptedAdvisor>();  // next = nullopt: abstain
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("tu1", "write", nlohmann::json::object(), ctx).is_error);
    CHECK(f.tool->calls.size() == 1);
    REQUIRE(inner_ptr->seen.size() == 1);  // the inner checker -- the human -- decided
}

TEST_CASE("a throwing advisor degrades to the wire prompt, never a lost turn",
          "[permission_advisor]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    auto advisor = std::make_shared<ScriptedAdvisor>();
    advisor->throw_std = true;
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("tu1", "write", nlohmann::json::object(), ctx).is_error);
    REQUIRE(inner_ptr->seen.size() == 1);

    // Non-standard exceptions too: anything escaping Advise must not escape check().
    advisor->throw_std = false;
    advisor->throw_unknown = true;
    CHECK_FALSE(executor.Run("tu2", "write", nlohmann::json::object(), ctx).is_error);
    REQUIRE(inner_ptr->seen.size() == 2);
}

TEST_CASE("under AllowOnly the advisor is never consulted", "[permission_advisor][run_mode]") {
    // Ask short-circuits to AlwaysDeny before the checker runs, so an
    // auto-approver cannot smuggle approvals into a headless run.
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    auto advisor = std::make_shared<ScriptedAdvisor>();
    advisor->next = PermissionResponse{PermissionDecision::Allow, {}};
    auto inner = std::make_unique<UnreachablePermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker, PermissionRunMode::AllowOnly);
    auto ctx = f.Context();

    CHECK(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK(advisor->seen.empty());
    CHECK_FALSE(inner_ptr->called);
}

TEST_CASE("grants returned by an advisor are filtered against the disclosure",
          "[permission_advisor][grants]") {
    // The double filter holds unchanged: an advisor may grant only keys the
    // disclosure offered. Returning an undisclosed key mints nothing, and the
    // next invocation prompts again.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));

    auto advisor = std::make_shared<ScriptedAdvisor>();
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    // First invocation: the advisor claims a grant on rm, which was never disclosed.
    advisor->next = PermissionResponse{PermissionDecision::Allow, {"bash:/bin/rm"}};
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "git status"}}, ctx).is_error);
    REQUIRE(executor.granted_count() == 0);  // the undisclosed key minted nothing
    REQUIRE(inner_ptr->seen.empty());  // the advisor's Allow decided; only the key was filtered

    // Second invocation: the advisor grants exactly what was disclosed.
    advisor->next = PermissionResponse{PermissionDecision::Allow, {"bash:/usr/bin/git"}};
    CHECK_FALSE(executor.Run("tu2", "bash", {{"command", "git log"}}, ctx).is_error);
    REQUIRE(executor.granted_count() == 1);
    CHECK(executor.granted_keys() == std::vector<std::string>{"bash:/usr/bin/git"});

    // The grant silences the third prompt: the advisor answers Allow with no
    // grant needed and the conjunction is already satisfied.
    const auto seen_before = advisor->seen.size();
    CHECK_FALSE(executor.Run("tu3", "bash", {{"command", "git diff"}}, ctx).is_error);
    CHECK(advisor->seen.size() == seen_before);  // short-circuit before the checker
    CHECK(inner_ptr->seen.empty());  // the inner checker was never needed in this test
}

TEST_CASE("a wedged advisor does not outlive cancellation", "[permission_advisor][cancel]") {
    using namespace std::chrono_literals;

    auto advisor = std::make_shared<ScriptedAdvisor>();
    std::atomic<bool> cancelled{false};
    auto saw_cancel = std::make_shared<std::atomic<bool>>(false);

    // The advisor contract: poll is_cancelled and return. This one spins on the
    // predicate exactly as a well-behaved advisor backed by a pollable decider
    // would; the test proves the predicate actually reaches it.
    struct Polling : tools::PermissionAdvisor {
        std::shared_ptr<std::atomic<bool>> observed;
        std::optional<PermissionResponse> Advise(const tools::PermissionRequest&,
                                                 const tools::AdvisorContext& ctx) override {
            while (!ctx.is_cancelled()) {
                std::this_thread::sleep_for(1ms);
            }
            observed->store(true);
            return std::nullopt;  // abstain on cancellation
        }
    };
    auto polling = std::make_shared<Polling>();
    polling->observed = saw_cancel;
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(polling, std::move(inner),
                                            [&cancelled] { return cancelled.load(); });

    minimal::testing::BackgroundResult<PermissionResponse> call(
        [&] { return checker.check(tools::PermissionRequest{}); });

    // Blocked while nobody cancels -- but not forever once we do.
    CHECK_FALSE(call.WaitFor(50ms));
    cancelled.store(true);
    CHECK(call.WaitFor(minimal::testing::kDefaultDeadline));
    CHECK(saw_cancel->load());
    // The abstention fell through to the inner checker rather than losing the turn.
    CHECK(call.Ready());
    CHECK(inner_ptr->seen.size() == 1);  // the advisor abstained; the fall-through answered
}

TEST_CASE("registry attributes registrations and refuses duplicates", "[permission_advisor]") {
    tools::PermissionAdvisorRegistry registry;
    auto first = std::make_shared<ScriptedAdvisor>();
    auto second = std::make_shared<ScriptedAdvisor>();

    // Outside any attribution window: refused -- an advisor the config could
    // never name is worse than no advisor.
    registry.Register(first);
    CHECK_FALSE(registry.Has("some-plugin"));
    CHECK(registry.attributions().empty());

    registry.SetAttribution("auto-approve");
    registry.Register(first);
    registry.ClearAttribution();
    CHECK(registry.Find("auto-approve") == first);
    REQUIRE(registry.attributions().size() == 1);
    CHECK(registry.attributions().front().plugin_name == "auto-approve");

    // Second registration from the same plugin: refused, first stays in force.
    registry.SetAttribution("auto-approve");
    registry.Register(second);
    registry.ClearAttribution();
    CHECK(registry.Find("auto-approve") == first);

    // A disabled plugin is a runtime condition: Find returns null, not an error.
    CHECK(registry.Find("no-such-plugin") == nullptr);
}
