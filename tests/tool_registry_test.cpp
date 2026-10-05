#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;
using namespace minimal::testing;

TEST_CASE("Register refuses names in another owner's reserved namespace",
          "[tool_registry][invariant]") {
    // A reserved namespace belongs to its owner (the MCP manager reserves
    // mcp__; a plugin reserves its own): its permission model claims those
    // names. A plugin registering "mcp__shell" would ride that model --
    // permission decisions for a group that does not exist -- so
    // registration from anyone but the owner must refuse rather than serve.
    // Core registrations (empty attribution: the process's own code, plus the
    // test harness's fake group tools) keep access.
    tools::ToolRegistry registry;
    REQUIRE(registry.ReserveNamespace("mcp__"));  // core-owned
    registry.SetAttribution("shady");
    registry.Register(std::make_unique<FakeTool>("mcp__shell", tools::ToolCategory::Execute));
    CHECK(registry.Find("mcp__shell") == nullptr);
    registry.ClearAttribution();

    // Core / unattributed registration is untouched.
    registry.Register(std::make_unique<FakeTool>("mcp__real", tools::ToolCategory::ReadOnly));
    CHECK(registry.Find("mcp__real") != nullptr);
    registry.Register(std::make_unique<FakeTool>("search", tools::ToolCategory::ReadOnly));
    CHECK(registry.Find("search") != nullptr);
}

TEST_CASE("ReserveNamespace: the owner may register in it, overlaps are refused",
          "[tool_registry][invariant]") {
    tools::ToolRegistry registry;
    registry.SetAttribution("web");
    REQUIRE(registry.ReserveNamespace("web__"));
    registry.Register(std::make_unique<FakeTool>("web__fetch", tools::ToolCategory::ReadOnly));
    registry.ClearAttribution();
    CHECK(registry.Find("web__fetch") != nullptr);
    CHECK(registry.IsReserved("web__anything"));
    CHECK_FALSE(registry.IsReserved("webfetch"));

    registry.SetAttribution("other");
    CHECK_FALSE(registry.ReserveNamespace("web__x__"));  // inside web__
    CHECK_FALSE(registry.ReserveNamespace("web"));       // contains web__
    registry.Register(std::make_unique<FakeTool>("web__steal", tools::ToolCategory::ReadOnly));
    registry.ClearAttribution();
    CHECK(registry.Find("web__steal") == nullptr);

    // Group tools: only inside a reserved namespace, and never in DefinitionsFor.
    std::vector<std::unique_ptr<tools::Tool>> batch;
    batch.push_back(std::make_unique<FakeTool>("web__g__a", tools::ToolCategory::ReadOnly));
    registry.ReplaceGroupTools("web__g__", std::move(batch));
    CHECK(registry.Find("web__g__a") != nullptr);
    CHECK(registry.DefinitionsForGroups({"web__g__"}).size() == 1);
    for (const auto& def : registry.DefinitionsFor({"*"})) CHECK(def.name.rfind("web__", 0) != 0);

    std::vector<std::unique_ptr<tools::Tool>> stray;
    stray.push_back(std::make_unique<FakeTool>("free__a", tools::ToolCategory::ReadOnly));
    registry.ReplaceGroupTools("free__", std::move(stray));
    CHECK(registry.Find("free__a") == nullptr);
}

TEST_CASE("Register keeps first-wins and parks the duplicate in shadowed_",
          "[tool_registry]") {
    tools::ToolRegistry registry;
    registry.Register(std::make_unique<FakeTool>("dup", tools::ToolCategory::ReadOnly));
    registry.Register(std::make_unique<FakeTool>("dup", tools::ToolCategory::Execute));

    const auto kept = registry.Find("dup");
    REQUIRE(kept != nullptr);
    CHECK(kept->spec().category == tools::ToolCategory::ReadOnly);
}

namespace {
// A tool that opts into Tool::default_dotenv_protected(), the way tools-fs's
// Read/Write/Edit do -- see fs_tools.cpp.
class DotenvProtectedFakeTool : public FakeTool {
public:
    using FakeTool::FakeTool;
    bool default_dotenv_protected() const override { return true; }
};
}  // namespace

TEST_CASE("PermissionKeys/DotenvProtectedPermissionKeys reflect the live registry, not a "
          "hardcoded name list",
          "[tool_registry]") {
    // Two tools, neither named read/write/edit: proves the structural query
    // works for a renamed/third-party tool exactly like a bundled one
    // (ARCHITECTURE.md, "Core vs. plugin surface" -- fixes the
    // "not structurally privileged" claim for real).
    tools::ToolRegistry registry;
    registry.Register(
        std::make_unique<DotenvProtectedFakeTool>("custom_read", tools::ToolCategory::ReadOnly));
    registry.Register(std::make_unique<FakeTool>("custom_search", tools::ToolCategory::ReadOnly));
    registry.SetPermissionKeys({});  // no aliasing: surface name == permission key

    const auto all = registry.PermissionKeys();
    CHECK(std::find(all.begin(), all.end(), "custom_read") != all.end());
    CHECK(std::find(all.begin(), all.end(), "custom_search") != all.end());

    const auto dotenv = registry.DotenvProtectedPermissionKeys();
    CHECK(dotenv == std::vector<std::string>{"custom_read"});
}

TEST_CASE("PermissionKeysByPlugin groups live permission keys by registering plugin",
          "[tool_registry][plugins]") {
    tools::ToolRegistry registry;
    registry.SetAttribution("a");
    registry.Register(std::make_unique<FakeTool>("shared", tools::ToolCategory::ReadOnly));
    registry.Register(std::make_unique<FakeTool>("x", tools::ToolCategory::ReadOnly));
    registry.SetAttribution("b");
    registry.Register(std::make_unique<FakeTool>("shared", tools::ToolCategory::ReadOnly));
    registry.Register(std::make_unique<FakeTool>("y", tools::ToolCategory::ReadOnly));
    registry.SetAttribution("empty");  // a plugin whose tools are all core-reserved: none
    registry.ClearAttribution();
    registry.Register(std::make_unique<FakeTool>("core_tool", tools::ToolCategory::ReadOnly));
    REQUIRE(registry.ReserveNamespace("grp__"));
    std::vector<std::unique_ptr<tools::Tool>> batch;
    batch.push_back(std::make_unique<FakeTool>("grp__t", tools::ToolCategory::ReadOnly));
    registry.ReplaceGroupTools("grp__", std::move(batch));

    // What name resolution produces for a name two plugins provide: b's copy
    // served under an alias, both keyed by their qualified names.
    registry.ApplyAliases({{"b.shared", "b_shared"}});
    registry.SetPermissionKeys({{"shared", "a.shared"}, {"b_shared", "b.shared"}});

    const auto by_plugin = registry.PermissionKeysByPlugin();
    CHECK(by_plugin.at("a") == std::vector<std::string>{"a.shared", "x"});
    CHECK(by_plugin.at("b") == std::vector<std::string>{"b.shared", "y"});
    // Core built-ins and group (MCP) tools belong to no plugin group.
    CHECK(by_plugin.count("") == 0);
    CHECK(by_plugin.size() == 2);
}
