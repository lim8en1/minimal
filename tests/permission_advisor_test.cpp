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
    std::thread::id advise_thread;  // which thread Advise ran on

    std::optional<PermissionResponse> Advise(const tools::PermissionRequest& request,
                                             const tools::AdvisorContext&) override {
        advise_thread = std::this_thread::get_id();
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

TEST_CASE("an advisor never mints session grants", "[permission_advisor][grants]") {
    // An advisor decides ONE invocation. Its granted_keys -- disclosed or not
    // -- are discarded, so every later call is judged again: a blanket grant
    // from one confident allow of `git status` must not silence the judgement
    // of `git push --force`.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));

    auto advisor = std::make_shared<ScriptedAdvisor>();
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();

    tools::AdvisedPermissionChecker checker(advisor, std::move(inner), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    // An undisclosed key mints nothing (as before).
    advisor->next = PermissionResponse{PermissionDecision::Allow, {"bash:/bin/rm"}};
    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "git status"}}, ctx).is_error);
    REQUIRE(executor.granted_count() == 0);
    REQUIRE(inner_ptr->seen.empty());  // the advisor's Allow decided

    // A DISCLOSED key mints nothing either.
    advisor->next = PermissionResponse{PermissionDecision::Allow, {"bash:/usr/bin/git"}};
    CHECK_FALSE(executor.Run("tu2", "bash", {{"command", "git log"}}, ctx).is_error);
    REQUIRE(executor.granted_count() == 0);

    // So the next call reaches the advisor again.
    const auto seen_before = advisor->seen.size();
    advisor->next = PermissionResponse{PermissionDecision::Allow, {}};
    CHECK_FALSE(executor.Run("tu3", "bash", {{"command", "git push --force"}}, ctx).is_error);
    CHECK(advisor->seen.size() == seen_before + 1);
    CHECK(inner_ptr->seen.empty());
}

TEST_CASE("a human's grant still passes through the advised checker",
          "[permission_advisor][grants]") {
    // Abstain falls through to the inner checker; ITS granted_keys are the
    // human's and are remembered as before.
    Fixture f;
    auto& tool = f.AddTool("bash", ToolCategory::Execute);
    tool.set_disclosure(Disclosing({"bash:/usr/bin/git"}));

    struct Abstaining : tools::PermissionAdvisor {
        std::optional<PermissionResponse> Advise(const tools::PermissionRequest&,
                                                 const tools::AdvisorContext&) override {
            return std::nullopt;
        }
    };
    struct Granting : tools::PermissionChecker {
        PermissionResponse check(const tools::PermissionRequest&) override {
            return PermissionResponse{PermissionDecision::Allow, {"bash:/usr/bin/git"}};
        }
    };
    tools::AdvisedPermissionChecker checker(std::make_shared<Abstaining>(),
                                            std::make_unique<Granting>(), [] { return false; });
    tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
    auto ctx = f.Context();

    CHECK_FALSE(executor.Run("tu1", "bash", {{"command", "git status"}}, ctx).is_error);
    CHECK(executor.granted_keys() == std::vector<std::string>{"bash:/usr/bin/git"});
}

TEST_CASE("a wedged advisor does not outlive cancellation", "[permission_advisor][cancel]") {
    using namespace std::chrono_literals;

    // Both flags live on the heap on purpose: the checker's abandon path
    // DETACHES the advisor thread (permission_advisor.cpp's poll loop), and a
    // detached thread can still be inside ctx.is_cancelled() when this scope
    // exits -- a stack atomic captured by reference would be a dangling read
    // under exactly the starvation this test simulates.
    auto cancelled = std::make_shared<std::atomic<bool>>(false);
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
                                            [cancelled] { return cancelled->load(); });

    minimal::testing::BackgroundResult<PermissionResponse> call(
        [&] { return checker.check(tools::PermissionRequest{}); });

    // Blocked while nobody cancels -- but not forever once we do.
    CHECK_FALSE(call.WaitFor(50ms));
    cancelled->store(true);
    // 10 s, not kDefaultDeadline's 2 s: the advisor's poll thread can be
    // starved past 2 s under an 8-way parallel ASan/TSan ctest on a loaded
    // machine -- the invariant under test is "returns once cancelled", not
    // "returns within 2 s". (Observed flaking exactly there.)
    CHECK(call.WaitFor(10s));
    // The checker's abandon path does not wait for the advisor to NOTICE the
    // cancel -- it detaches the thread and falls through. That detached 1 ms
    // poller races everything else in an 8-way parallel run, so whether it
    // has reached `observed->store` by the time WaitFor returns is a coin
    // flip. The invariant is "the cancel REACHES the advisor", not "reaches
    // it before the checker returns" -- poll with a deadline instead of
    // asserting a race outcome.
    REQUIRE(minimal::testing::WaitForDeadline(10s, [&] { return saw_cancel->load(); }));
    // The abstention fell through to the inner checker rather than losing the turn.
    CHECK(call.Ready());
    CHECK(inner_ptr->seen.size() == 1);  // the advisor abstained; the fall-through answered
}

TEST_CASE("a wedged advisor that ignores cancellation costs only its budget",
          "[permission_advisor][timeout]") {
    using namespace std::chrono_literals;

    // The contract-breaking case: an advisor that NEVER returns -- no polling,
    // no answer. Before the budget existed this wedged the session worker, and
    // through it Terminate, the reaper and process shutdown. Now it costs one
    // bounded check and the human decides, as if the advisor had abstained.
    struct Wedged : tools::PermissionAdvisor {
        std::optional<PermissionResponse> Advise(const tools::PermissionRequest&,
                                                 const tools::AdvisorContext&) override {
            std::this_thread::sleep_for(60s);  // ignores is_cancelled entirely
            return std::nullopt;
        }
    };
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();
    tools::AdvisedPermissionChecker checker(std::make_shared<Wedged>(), std::move(inner),
                                            [] { return false; },
                                            std::chrono::milliseconds{300});

    minimal::testing::BackgroundResult<PermissionResponse> call(
        [&] { return checker.check(tools::PermissionRequest{}); });

    // Returns within a ceiling the budget dictates, not after the 60s sleep.
    CHECK(call.WaitFor(5s));
    REQUIRE(inner_ptr->seen.size() == 1);  // the budget = fall-through to the human
}

TEST_CASE("cancellation abandons the bounded wait before the budget is spent",
          "[permission_advisor][timeout]") {
    using namespace std::chrono_literals;

    struct Wedged : tools::PermissionAdvisor {
        std::optional<PermissionResponse> Advise(const tools::PermissionRequest&,
                                                 const tools::AdvisorContext&) override {
            std::this_thread::sleep_for(60s);
            return std::nullopt;
        }
    };
    std::atomic<bool> cancelled{false};
    auto inner = std::make_unique<AlwaysAllowPermissionChecker>();
    auto* inner_ptr = inner.get();
    tools::AdvisedPermissionChecker checker(std::make_shared<Wedged>(), std::move(inner),
                                            [&cancelled] { return cancelled.load(); },
                                            std::chrono::milliseconds{10000});

    minimal::testing::BackgroundResult<PermissionResponse> call(
        [&] { return checker.check(tools::PermissionRequest{}); });

    std::this_thread::sleep_for(50ms);
    cancelled.store(true);
    // Promptly -- milliseconds, not the 10s budget -- because Terminate cannot
    // afford to wait out an advisor that will never answer.
    CHECK(call.WaitFor(2s));
    CHECK(inner_ptr->seen.size() == 1);
}

TEST_CASE("timeout 0 keeps Advise on the calling thread; a budget moves it off",
          "[permission_advisor][timeout]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    auto ctx = f.Context();

    // Disabled budget: the pre-timeout shape, Advise synchronous.
    auto advisor = std::make_shared<ScriptedAdvisor>();
    advisor->next = PermissionResponse{PermissionDecision::Allow, {}};
    {
        tools::AdvisedPermissionChecker checker(advisor, std::make_unique<UnreachablePermissionChecker>(),
                                                [] { return false; },
                                                std::chrono::milliseconds{0});
        tools::ToolExecutor executor = MakeExecutor(f.registry, checker);
        CHECK_FALSE(executor.Run("tu1", "write", nlohmann::json::object(), ctx).is_error);
        CHECK(advisor->advise_thread == std::this_thread::get_id());
    }

    // Budgeted: Advise runs on its own thread; the worker only waits.
    auto advisor2 = std::make_shared<ScriptedAdvisor>();
    advisor2->next = PermissionResponse{PermissionDecision::Allow, {}};
    tools::AdvisedPermissionChecker checker2(advisor2, std::make_unique<UnreachablePermissionChecker>(),
                                             [] { return false; },
                                             std::chrono::milliseconds{2000});
    tools::ToolExecutor executor2 = MakeExecutor(f.registry, checker2);
    CHECK_FALSE(executor2.Run("tu2", "write", nlohmann::json::object(), ctx).is_error);
    REQUIRE(f.tool->calls.size() == 2);
    CHECK(advisor2->advise_thread != std::this_thread::get_id());
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

TEST_CASE("a child's advisor cannot exceed its parent's ceiling", "[permission_advisor][ceiling]") {
    // The advisor sits BELOW the ceilings in ToolExecutor::ResolveMode: a
    // policy resolved to AlwaysDeny (the child's inherited ceiling) never
    // reaches the checker, so the advisor is never even consulted. An
    // auto-approver therefore cannot buy back what the parent denied.
    Fixture f;
    f.AddTool("bash", ToolCategory::Execute);
    auto advisor = std::make_shared<ScriptedAdvisor>();
    advisor->next = PermissionResponse{PermissionDecision::Allow, {}};
    UnreachablePermissionChecker inner;

    tools::AdvisedPermissionChecker checker(advisor,
                                            std::make_unique<UnreachablePermissionChecker>(inner),
                                            [] { return false; });
    tools::PermissionPolicy ceiling;
    ceiling.execute = PermissionMode::AlwaysDeny;
    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(), {},
                                 PermissionRunMode::Normal, ceiling);
    auto ctx = f.Context();

    CHECK(executor.Run("tu1", "bash", nlohmann::json::object(), ctx).is_error);
    CHECK(f.tool->calls.empty());
    CHECK(advisor->seen.empty());  // denied before the advisor ever saw it
}

TEST_CASE("the advisor thread counter returns to zero on the normal path and never "
          "drifts negative",
          "[permission_advisor][invariant]") {
    using namespace std::chrono_literals;

    // The regression: the counter incremented at ABANDON but decremented on
    // EVERY task exit, so normal completions drove it negative -- and each
    // drifted decrement then offset a real abandoned increment, quietly
    // defeating the kMaxAbandonedAdvisors ceiling. Counting from launch makes
    // the normal path net zero.
    tools::AdvisedPermissionChecker checker(
        std::make_shared<ScriptedAdvisor>(),  // answers instantly, every time
        std::make_unique<AlwaysAllowPermissionChecker>(),
        [] { return false; }, std::chrono::milliseconds{50});

    for (int i = 0; i < 20; ++i) {
        (void)checker.check(tools::PermissionRequest{});
        CHECK(checker.in_flight_advisors() == 0);
    }
}

TEST_CASE("abandoned advisor threads stay counted until they exit",
          "[permission_advisor][invariant]") {
    using namespace std::chrono_literals;

    // The ceiling's subject: threads launched and still running. A wedged
    // advisor abandoned at its budget remains counted -- two abandons leave
    // two in flight, which is what stops an every-call wedge from
    // accumulating threads unboundedly.
    struct Wedged : tools::PermissionAdvisor {
        std::optional<PermissionResponse> Advise(const tools::PermissionRequest&,
                                                 const tools::AdvisorContext&) override {
            std::this_thread::sleep_for(30s);  // ignores is_cancelled entirely
            return std::nullopt;
        }
    };
    tools::AdvisedPermissionChecker checker(std::make_shared<Wedged>(),
                                            std::make_unique<AlwaysAllowPermissionChecker>(),
                                            [] { return false; },
                                            std::chrono::milliseconds{40});

    (void)checker.check(tools::PermissionRequest{});  // burns the budget, abandons
    CHECK(checker.in_flight_advisors() == 1);
    (void)checker.check(tools::PermissionRequest{});  // abandons a second thread
    CHECK(checker.in_flight_advisors() == 2);
}
