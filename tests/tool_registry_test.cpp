#include <catch2/catch_test_macros.hpp>

#include <memory>

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
