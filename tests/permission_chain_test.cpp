#include <catch2/catch_test_macros.hpp>

#include "minimal/tools/permission.hpp"

using namespace minimal::tools;

TEST_CASE("permission chain: preset per-tool override wins over everything", "[permission][chain]") {
    PermissionOverrideLayer preset;
    preset.per_tool_overrides["bash"] = PermissionMode::AlwaysAllow;
    preset.execute = PermissionMode::AlwaysDeny;
    PermissionOverrideLayer global;
    global.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;
    global.execute = PermissionMode::AlwaysDeny;

    CHECK(resolve_permission_chain("bash", ToolCategory::Execute, preset, global) == PermissionMode::AlwaysAllow);
}

TEST_CASE("permission chain: preset category beats global layers", "[permission][chain]") {
    PermissionOverrideLayer preset;
    preset.file_write = PermissionMode::AlwaysAllow;
    PermissionOverrideLayer global;
    global.per_tool_overrides["write"] = PermissionMode::AlwaysDeny;
    global.file_write = PermissionMode::AlwaysDeny;

    CHECK(resolve_permission_chain("write", ToolCategory::FileWrite, preset, global) == PermissionMode::AlwaysAllow);
}

TEST_CASE("permission chain: global per-tool override beats global category", "[permission][chain]") {
    PermissionOverrideLayer preset;  // empty -- falls through entirely
    PermissionOverrideLayer global;
    global.per_tool_overrides["write"] = PermissionMode::AlwaysAllow;
    global.file_write = PermissionMode::AlwaysDeny;

    CHECK(resolve_permission_chain("write", ToolCategory::FileWrite, preset, global) == PermissionMode::AlwaysAllow);
}

TEST_CASE("permission chain: global category is the last configurable layer", "[permission][chain]") {
    PermissionOverrideLayer preset;
    PermissionOverrideLayer global;
    global.execute = PermissionMode::AlwaysAllow;

    CHECK(resolve_permission_chain("anything", ToolCategory::Execute, preset, global) == PermissionMode::AlwaysAllow);
}

TEST_CASE("permission chain: falls all the way to hard-coded default", "[permission][chain]") {
    PermissionOverrideLayer preset;
    PermissionOverrideLayer global;

    CHECK(resolve_permission_chain("anything", ToolCategory::ReadOnly, preset, global) == PermissionMode::AlwaysAllow);
    CHECK(resolve_permission_chain("anything", ToolCategory::FileWrite, preset, global) == PermissionMode::Ask);
    CHECK(resolve_permission_chain("anything", ToolCategory::Execute, preset, global) == PermissionMode::Ask);
}

TEST_CASE("out-of-workdir chain", "[permission][chain]") {
    PermissionOverrideLayer preset, global;
    CHECK(resolve_out_of_workdir_chain(preset, global) == PermissionMode::Ask);

    global.out_of_workdir = PermissionMode::AlwaysAllow;
    CHECK(resolve_out_of_workdir_chain(preset, global) == PermissionMode::AlwaysAllow);

    preset.out_of_workdir = PermissionMode::AlwaysDeny;
    CHECK(resolve_out_of_workdir_chain(preset, global) == PermissionMode::AlwaysDeny);
}

TEST_CASE("network chain", "[permission][chain]") {
    PermissionOverrideLayer preset, global;
    CHECK(resolve_network_chain(preset, global) == PermissionMode::Ask);

    global.network = PermissionMode::AlwaysAllow;
    CHECK(resolve_network_chain(preset, global) == PermissionMode::AlwaysAllow);

    preset.network = PermissionMode::AlwaysDeny;
    CHECK(resolve_network_chain(preset, global) == PermissionMode::AlwaysDeny);
}

TEST_CASE("mcp permission chain", "[permission][chain][mcp]") {
    CHECK(resolve_mcp_chain(std::nullopt, PermissionMode::Ask) == PermissionMode::Ask);
    CHECK(resolve_mcp_chain(std::nullopt, PermissionMode::AlwaysAllow) == PermissionMode::AlwaysAllow);
    CHECK(resolve_mcp_chain(PermissionMode::AlwaysDeny, PermissionMode::AlwaysAllow) == PermissionMode::AlwaysDeny);
}
