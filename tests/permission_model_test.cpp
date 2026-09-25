#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <stdexcept>

#include "fakes.hpp"
#include "minimal/config/permissions_config.hpp"
#include "minimal/tools/permission_model.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;
using namespace minimal::testing;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionRunMode;
using minimal::tools::PermissionStage;
using minimal::tools::ToolCategory;

namespace {

// A model's layer: the base mode and (optionally) the network mode it answers.
struct FakeLayer {
    PermissionMode base = PermissionMode::Ask;
    PermissionMode network = PermissionMode::Ask;
};

class FakeModel final : public tools::PermissionModel {
public:
    FakeModel(std::set<std::string> claims, unsigned replaces)
        : claims_(std::move(claims)), replaces_(replaces) {}

    unsigned Replaces() const override { return replaces_; }
    bool Claims(std::string_view key) const override {
        return claims_.count(std::string(key)) != 0;
    }

    // global/preset: {"base": mode, "network": mode}; an untrusted preset
    // may only narrow the global answer.
    tools::PermissionLayer BuildLayer(const nlohmann::json& global, const nlohmann::json& preset,
                                      bool preset_trusted) const override {
        FakeLayer layer;
        auto read = [](const nlohmann::json& slice, const char* key, PermissionMode& out) {
            if (!slice.is_object() || !slice.contains(key)) return std::optional<PermissionMode>{};
            auto mode = tools::mode_from_name(slice.at(key).get<std::string>());
            if (!mode) throw std::runtime_error(std::string("bad ") + key);
            return mode;
        };
        for (auto [key, field] : {std::pair{"base", &layer.base}, {"network", &layer.network}}) {
            if (auto g = read(global, key, *field)) *field = *g;
            if (auto p = read(preset, key, *field)) {
                *field = preset_trusted ? *p : tools::most_restrictive(*p, *field);
            }
        }
        return std::make_shared<const FakeLayer>(layer);
    }

    tools::PermissionAnswer Resolve(PermissionStage stage,
                                    const tools::PermissionQuery& query) const override {
        if (throws) throw std::runtime_error("boom");
        const FakeLayer layer =
            query.layer != nullptr ? *static_cast<const FakeLayer*>(query.layer) : FakeLayer{};
        const PermissionMode mode = stage == PermissionStage::kBase ? layer.base : layer.network;
        return {mode, "fake " + std::string(tools::mode_name(mode))};
    }

    bool throws = false;

private:
    std::set<std::string> claims_;
    unsigned replaces_;
};

tools::BoundPermissionModel Bound(std::shared_ptr<const FakeModel> model, FakeLayer layer) {
    return {"fake", std::move(model), std::make_shared<const FakeLayer>(layer)};
}

struct Fixture {
    tools::ToolRegistry registry;
    FakeTool* tool = nullptr;

    FakeTool& AddTool(std::string name, ToolCategory category) {
        auto owned = std::make_unique<FakeTool>(std::move(name), category);
        tool = owned.get();
        registry.Register(std::move(owned));
        return *tool;
    }

    tools::ToolContext Context() {
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = std::filesystem::temp_directory_path();
        return ctx;
    }
};

tools::PermissionPolicy AskAll() {
    tools::PermissionPolicy p;
    p.read_only = p.file_write = p.execute = p.out_of_workdir = p.network = PermissionMode::Ask;
    return p;
}

constexpr unsigned kBase = static_cast<unsigned>(PermissionStage::kBase);
constexpr unsigned kNetwork = static_cast<unsigned>(PermissionStage::kNetwork);

}  // namespace

TEST_CASE("PermissionModel: a claimed tool's base mode comes from the model",
          "[permission_model]") {
    Fixture f;
    f.AddTool("fetch", ToolCategory::Execute);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kBase);
    UnreachablePermissionChecker checker;
    // Core says ask for execute; the model owns the base stage and allows.
    tools::ToolExecutor executor(f.registry, checker, AskAll(),
                                 tools::PermissionModels{Bound(model, {PermissionMode::AlwaysAllow})},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto verdict = executor.Explain(*f.registry.Find("fetch"), nlohmann::json::object(), ctx);
    CHECK(verdict.mode == PermissionMode::AlwaysAllow);
    CHECK(verdict.reason == "fake allow");
    CHECK_FALSE(executor.Run("t1", "fetch", nlohmann::json::object(), ctx).is_error);
    CHECK_FALSE(checker.called);
}

TEST_CASE("PermissionModel: an unclaimed tool still resolves through the core model",
          "[permission_model]") {
    Fixture f;
    f.AddTool("write", ToolCategory::FileWrite);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kBase);
    tools::PermissionPolicy policy = AskAll();
    policy.file_write = PermissionMode::AlwaysDeny;
    UnreachablePermissionChecker checker;
    tools::ToolExecutor executor(f.registry, checker, policy,
                                 tools::PermissionModels{Bound(model, {PermissionMode::AlwaysAllow})},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK(executor.ResolveMode(*f.registry.Find("write"), nlohmann::json::object(), ctx) ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("PermissionModel: an operator per-tool deny beats the model's allow",
          "[permission_model]") {
    Fixture f;
    f.AddTool("fetch", ToolCategory::Execute);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kBase);
    tools::PermissionPolicy policy = AskAll();
    policy.per_tool_overrides["fetch"] = PermissionMode::AlwaysDeny;
    UnreachablePermissionChecker checker;
    tools::ToolExecutor executor(f.registry, checker, policy,
                                 tools::PermissionModels{Bound(model, {PermissionMode::AlwaysAllow})},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    const auto verdict = executor.Explain(*f.registry.Find("fetch"), nlohmann::json::object(), ctx);
    CHECK(verdict.mode == PermissionMode::AlwaysDeny);
    CHECK(verdict.reason == "per-tool rule fetch=deny");
}

TEST_CASE("PermissionModel: the parent's layer is a ceiling", "[permission_model]") {
    Fixture f;
    f.AddTool("fetch", ToolCategory::Execute);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kBase);
    UnreachablePermissionChecker checker;
    tools::ToolExecutor executor(
        f.registry, checker, AskAll(),
        tools::PermissionModels{Bound(model, {PermissionMode::AlwaysAllow})},
        PermissionRunMode::Normal, AskAll(),
        tools::PermissionModels{Bound(model, {PermissionMode::AlwaysDeny})});
    auto ctx = f.Context();
    const auto verdict = executor.Explain(*f.registry.Find("fetch"), nlohmann::json::object(), ctx);
    CHECK(verdict.mode == PermissionMode::AlwaysDeny);
    CHECK(verdict.reason == "parent session: fake deny");
}

TEST_CASE("PermissionModel: the parent's per-tool deny holds for a modelled tool",
          "[permission_model]") {
    Fixture f;
    f.AddTool("fetch", ToolCategory::Execute);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kBase);
    tools::PermissionPolicy parent = AskAll();
    parent.per_tool_overrides["fetch"] = PermissionMode::AlwaysDeny;
    UnreachablePermissionChecker checker;
    tools::ToolExecutor executor(
        f.registry, checker, AskAll(),
        tools::PermissionModels{Bound(model, {PermissionMode::AlwaysAllow})},
        PermissionRunMode::Normal, parent,
        tools::PermissionModels{Bound(model, {PermissionMode::AlwaysAllow})});
    auto ctx = f.Context();
    CHECK(executor.ResolveMode(*f.registry.Find("fetch"), nlohmann::json::object(), ctx) ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("PermissionModel: a throwing model fails closed", "[permission_model]") {
    Fixture f;
    f.AddTool("fetch", ToolCategory::ReadOnly);
    auto model = std::make_shared<FakeModel>(std::set<std::string>{"fetch"}, kBase);
    model->throws = true;
    UnreachablePermissionChecker checker;
    tools::ToolExecutor executor(f.registry, checker, AllowAllPolicy(),
                                 tools::PermissionModels{Bound(model, {})},
                                 PermissionRunMode::Bypass);
    auto ctx = f.Context();
    const auto verdict = executor.Explain(*f.registry.Find("fetch"), nlohmann::json::object(), ctx);
    // Deny survives Bypass: the run mode only washes Ask.
    CHECK(verdict.mode == PermissionMode::AlwaysDeny);
    CHECK(verdict.reason == "permission model 'fake' failed: boom");
}

TEST_CASE("PermissionModel: run modes still wrap the model's answer", "[permission_model]") {
    Fixture f;
    f.AddTool("fetch", ToolCategory::Execute);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kBase);
    UnreachablePermissionChecker checker;
    auto ctx = f.Context();
    const tools::PermissionModels models{Bound(model, {PermissionMode::Ask})};

    tools::ToolExecutor bypass(f.registry, checker, AskAll(), models, PermissionRunMode::Bypass);
    CHECK(bypass.ResolveMode(*f.registry.Find("fetch"), nlohmann::json::object(), ctx) ==
          PermissionMode::AlwaysAllow);
    tools::ToolExecutor allow_only(f.registry, checker, AskAll(), models,
                                   PermissionRunMode::AllowOnly);
    CHECK(allow_only.ResolveMode(*f.registry.Find("fetch"), nlohmann::json::object(), ctx) ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("PermissionModel: a network model replaces the network fold only",
          "[permission_model]") {
    Fixture f;
    auto& tool = f.AddTool("fetch", ToolCategory::ReadOnly);
    tool.set_network_access_required(true);
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{"fetch"}, kNetwork);
    UnreachablePermissionChecker checker;
    auto ctx = f.Context();
    tools::PermissionPolicy policy = AskAll();
    policy.read_only = PermissionMode::AlwaysAllow;  // base stays core: allow

    SECTION("the model's allow lifts core's network ask") {
        tools::ToolExecutor executor(
            f.registry, checker, policy,
            tools::PermissionModels{Bound(model, {PermissionMode::Ask, PermissionMode::AlwaysAllow})},
            PermissionRunMode::Normal);
        CHECK(executor.ResolveMode(tool, nlohmann::json::object(), ctx) ==
              PermissionMode::AlwaysAllow);
    }
    SECTION("the model's deny tightens, explained as network access") {
        tools::ToolExecutor executor(
            f.registry, checker, policy,
            tools::PermissionModels{Bound(model, {PermissionMode::Ask, PermissionMode::AlwaysDeny})},
            PermissionRunMode::Normal);
        const auto verdict = executor.Explain(tool, nlohmann::json::object(), ctx);
        CHECK(verdict.mode == PermissionMode::AlwaysDeny);
        CHECK(verdict.reason == "network access: fake deny");
    }
    SECTION("core's base stage still decides first") {
        tools::PermissionPolicy denying = policy;
        denying.read_only = PermissionMode::AlwaysDeny;
        tools::ToolExecutor executor(
            f.registry, checker, denying,
            tools::PermissionModels{Bound(model, {PermissionMode::Ask, PermissionMode::AlwaysAllow})},
            PermissionRunMode::Normal);
        CHECK(executor.ResolveMode(tool, nlohmann::json::object(), ctx) ==
              PermissionMode::AlwaysDeny);
    }
}

TEST_CASE("PermissionModel: an executor always carries the MCP model while MCP is core",
          "[permission_model][mcp]") {
    Fixture f;
    tools::ToolRegistry& registry = f.registry;
    std::vector<std::unique_ptr<tools::Tool>> batch;
    batch.push_back(std::make_unique<FakeTool>("mcp__gh__get_issue", ToolCategory::ReadOnly));
    registry.ReplaceMcpTools("gh", std::move(batch));
    UnreachablePermissionChecker checker;
    // AllowAll would allow a ReadOnly tool; an MCP tool must still Ask
    // (the MCP model's default), never fall through to the category.
    tools::ToolExecutor executor(registry, checker, AllowAllPolicy(), tools::PermissionModels{},
                                 PermissionRunMode::Normal);
    auto ctx = f.Context();
    CHECK(executor.ResolveMode(*registry.Find("mcp__gh__get_issue"), nlohmann::json::object(),
                               ctx) == PermissionMode::Ask);
}

TEST_CASE("PermissionModelRegistry: keyed by plugin, one model each", "[permission_model]") {
    tools::PermissionModelRegistry registry;
    auto model = std::make_shared<const FakeModel>(std::set<std::string>{}, kBase);
    CHECK_FALSE(registry.Register(model));  // no attribution: no owner to key on

    registry.SetAttribution("tools-web");
    CHECK(registry.Register(model));
    CHECK_FALSE(registry.Register(model));
    registry.ClearAttribution();
    REQUIRE(registry.collisions().size() == 1);
    CHECK(registry.collisions().front().kind == "tools-web");
}

TEST_CASE("PermissionModelRegistry: Bind hands each model its slices", "[permission_model]") {
    tools::PermissionModelRegistry registry;
    registry.SetAttribution("tools-web");
    registry.Register(std::make_shared<const FakeModel>(std::set<std::string>{}, kBase));
    registry.ClearAttribution();

    const nlohmann::json global = {{"tools-web", {{"base", "ask"}}}, {"other", {{"base", "deny"}}}};
    const nlohmann::json preset = {{"tools-web", {{"base", "allow"}}}};

    auto layer_of = [](const tools::PermissionModels& models) {
        REQUIRE(models.size() == 1);
        CHECK(models.front().name == "tools-web");
        return *static_cast<const FakeLayer*>(models.front().layer.get());
    };
    CHECK(layer_of(registry.Bind(global, preset, true)).base == PermissionMode::AlwaysAllow);
    // A project preset may only narrow the operator's slice.
    CHECK(layer_of(registry.Bind(global, preset, false)).base == PermissionMode::Ask);

    const nlohmann::json bad = {{"tools-web", {{"base", "sometimes"}}}};
    try {
        registry.Bind(global, bad, true);
        FAIL("expected a throw");
    } catch (const std::exception& e) {
        CHECK(std::string(e.what()) == "permission model 'tools-web': bad base");
    }
}

TEST_CASE("PermissionModelRegistry: a model may only claim its own plugin's tools",
          "[permission_model]") {
    tools::ToolRegistry tools;
    tools.SetAttribution("tools-web");
    tools.Register(std::make_unique<FakeTool>("web_fetch", ToolCategory::ReadOnly));
    tools.ClearAttribution();
    tools.SetAttribution("tools-bash");
    tools.Register(std::make_unique<FakeTool>("bash", ToolCategory::Execute));
    tools.ClearAttribution();

    tools::PermissionModelRegistry models;
    SECTION("its own tool: clean") {
        models.SetAttribution("tools-web");
        models.Register(std::make_shared<const FakeModel>(std::set<std::string>{"web_fetch"}, kBase));
        models.ClearAttribution();
        CHECK(models.ValidateClaims(tools).empty());
    }
    SECTION("a foreign tool: refused, naming both") {
        models.SetAttribution("tools-web");
        models.Register(std::make_shared<const FakeModel>(
            std::set<std::string>{"web_fetch", "bash"}, kBase));
        models.ClearAttribution();
        const auto errors = models.ValidateClaims(tools);
        REQUIRE(errors.size() == 1);
        CHECK(errors.front() ==
              "permission model 'tools-web' (plugin 'tools-web') claims tool 'bash' registered "
              "by plugin 'tools-bash' -- a model may only govern its own plugin's tools");
    }
    SECTION("two claimants: refused") {
        models.SetAttribution("tools-web");
        models.Register(std::make_shared<const FakeModel>(std::set<std::string>{"web_fetch"}, kBase));
        models.ClearAttribution();
        models.SetAttribution("sneaky");
        models.Register(std::make_shared<const FakeModel>(std::set<std::string>{"web_fetch"}, kBase));
        models.ClearAttribution();
        const auto errors = models.ValidateClaims(tools);
        REQUIRE(errors.size() == 1);
        CHECK(errors.front().find("claimed by several permission models (sneaky, tools-web)") !=
              std::string::npos);
    }
}

TEST_CASE("MCP permission model: slices build the same policy as the legacy keys",
          "[permission_model][mcp]") {
    const auto model = tools::MakeMcpPermissionModel();
    auto policy_of = [&](const nlohmann::json& global, const nlohmann::json& preset, bool trusted) {
        const auto layer = model->BuildLayer(global, preset, trusted);
        return *static_cast<const tools::McpPermissionPolicy*>(layer.get());
    };
    const nlohmann::json global = {{"default", "deny"}};
    const nlohmann::json preset = {{"servers", {{"gh", "allow"}}}};

    const auto trusted = policy_of(global, preset, true);
    CHECK(trusted.default_mode == PermissionMode::AlwaysDeny);
    CHECK(trusted.mode_for("gh") == PermissionMode::AlwaysAllow);
    CHECK(policy_of(global, preset, false).mode_for("gh") == PermissionMode::AlwaysDeny);
    CHECK(policy_of(nullptr, nullptr, true).default_mode == PermissionMode::Ask);
    CHECK_THROWS(policy_of({{"default", "maybe"}}, nullptr, true));
    CHECK(model->Claims("mcp__gh__get_issue"));
    CHECK_FALSE(model->Claims("bash"));
    CHECK(model->Describe(model->BuildLayer(global, preset, true).get()) == "default=deny gh=allow");
}

TEST_CASE("permissions.json: plugins carries model slices verbatim", "[permission_model]") {
    const auto config = config::ParsePermissionsConfig(
        {{"plugins", {{"tools-web", {{"hosts", {{"*.internal", "deny"}}}}}}}});
    CHECK(config.plugin_slices["tools-web"]["hosts"]["*.internal"] == "deny");
    CHECK(config.unknown_keys.empty());
    CHECK_THROWS(config::ParsePermissionsConfig({{"plugins", "tools-web"}}));
}
