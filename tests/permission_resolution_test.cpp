#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/permission_resolution.hpp"

using namespace minimal::agent;
using minimal::tools::PermissionMode;
using minimal::tools::PermissionOverrideLayer;
using minimal::tools::ToolCategory;

TEST_CASE("BuildPermissionPolicy: preset category wins over global category", "[permission_resolution]") {
    PermissionOverrideLayer preset;
    preset.file_write = PermissionMode::AlwaysAllow;
    PermissionOverrideLayer global;
    global.file_write = PermissionMode::AlwaysDeny;

    auto policy = BuildPermissionPolicy(preset, global);
    CHECK(policy.file_write == PermissionMode::AlwaysAllow);
}

TEST_CASE("BuildPermissionPolicy: preset per-tool override wins over everything", "[permission_resolution]") {
    PermissionOverrideLayer preset;
    preset.execute = PermissionMode::AlwaysDeny;
    preset.per_tool_overrides["bash"] = PermissionMode::AlwaysAllow;
    PermissionOverrideLayer global;
    global.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;

    auto policy = BuildPermissionPolicy(preset, global);
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysAllow);
}

TEST_CASE("BuildPermissionPolicy: a global-only per-tool override is carried into the flat policy",
          "[permission_resolution]") {
    // Documented simplification: this tool has no preset-level opinion at all
    // (neither a per-tool override nor -- in the true chain -- would a preset
    // category even apply, since the global per-tool rule is more specific).
    // BuildPermissionPolicy treats any explicit per-tool rule, from either
    // layer, as outranking a preset's blanket category default.
    PermissionOverrideLayer preset;
    preset.execute = PermissionMode::AlwaysAllow;  // preset's blanket category default
    PermissionOverrideLayer global;
    global.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;

    auto policy = BuildPermissionPolicy(preset, global);
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysDeny);
    // An unrelated tool in the same category still gets the preset's category default.
    CHECK(policy.mode_for("other", ToolCategory::Execute) == PermissionMode::AlwaysAllow);
}

TEST_CASE("BuildPermissionPolicy: falls to hard-coded default when both layers are empty",
          "[permission_resolution]") {
    auto policy = BuildPermissionPolicy({}, {});
    CHECK(policy.mode_for("anything", ToolCategory::ReadOnly) == PermissionMode::AlwaysAllow);
    CHECK(policy.mode_for("anything", ToolCategory::FileWrite) == PermissionMode::Ask);
    CHECK(policy.mode_for("anything", ToolCategory::Execute) == PermissionMode::Ask);
}

TEST_CASE("BuildPermissionPolicy: out_of_workdir is wired through", "[permission_resolution]") {
    PermissionOverrideLayer preset;
    preset.out_of_workdir = PermissionMode::AlwaysDeny;
    auto policy = BuildPermissionPolicy(preset, {});
    CHECK(policy.out_of_workdir == PermissionMode::AlwaysDeny);
}

TEST_CASE("BuildPermissionPolicy: network is wired through", "[permission_resolution]") {
    PermissionOverrideLayer preset;
    preset.network = PermissionMode::AlwaysDeny;
    auto policy = BuildPermissionPolicy(preset, {});
    CHECK(policy.network == PermissionMode::AlwaysDeny);
}

TEST_CASE("BuildMcpPermissionPolicy: trusted preset per-server wins", "[permission_resolution]") {
    auto policy = BuildMcpPermissionPolicy({{"search", PermissionMode::AlwaysAllow}},
                                           PermissionMode::AlwaysDeny, true);
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysAllow);
}

TEST_CASE("BuildMcpPermissionPolicy: unset server falls to global default", "[permission_resolution]") {
    auto policy = BuildMcpPermissionPolicy({}, PermissionMode::AlwaysDeny, true);
    CHECK(policy.mode_for("unconfigured") == PermissionMode::AlwaysDeny);
}

TEST_CASE("BuildMcpPermissionPolicy: an untrusted preset may not widen the global default",
          "[permission_resolution]") {
    // Allow under a Deny floor folds to Deny; Allow under Ask folds to Ask.
    // The clamp is what stops a repo-shipped preset from auto-approving MCP.
    auto policy = BuildMcpPermissionPolicy({{"search", PermissionMode::AlwaysAllow}},
                                           PermissionMode::AlwaysDeny, false);
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysDeny);

    auto ask_floor = BuildMcpPermissionPolicy({{"search", PermissionMode::AlwaysAllow}},
                                              PermissionMode::Ask, false);
    CHECK(ask_floor.mode_for("search") == PermissionMode::Ask);
}

TEST_CASE("BuildMcpPermissionPolicy: an untrusted preset may still narrow", "[permission_resolution]") {
    // Deny under an Allow default stays Deny: narrowing is the whole point of
    // letting an untrusted preset speak at all.
    auto policy = BuildMcpPermissionPolicy({{"search", PermissionMode::AlwaysDeny}},
                                           PermissionMode::AlwaysAllow, false);
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysDeny);
}
