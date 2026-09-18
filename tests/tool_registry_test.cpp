#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "fakes.hpp"
#include "minimal/tools/tool_registry.hpp"

using namespace minimal;
using namespace minimal::testing;

TEST_CASE("Register refuses mcp__-prefixed names from a PLUGIN attribution",
          "[tool_registry][invariant]") {
    // The mcp__ namespace belongs to the MCP manager (ReplaceMcpTools is its
    // only writer) and the executor routes every mcp__ name through the
    // server-scoped permission chain. A plugin registering "mcp__shell" would
    // ride that chain -- permission decisions for a server that does not
    // exist -- so plugin-attributed registration must refuse rather than
    // serve. Core registrations (empty attribution: the process's own code,
    // plus the test harness's fake MCP tools) keep access.
    tools::ToolRegistry registry;
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
