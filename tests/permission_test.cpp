#include <catch2/catch_test_macros.hpp>

#include "minimal/tools/permission.hpp"

using namespace minimal::tools;

TEST_CASE("most_restrictive is the lattice meet", "[permission]") {
    CHECK(most_restrictive(PermissionMode::AlwaysAllow, PermissionMode::Ask) == PermissionMode::Ask);
    CHECK(most_restrictive(PermissionMode::Ask, PermissionMode::AlwaysDeny) == PermissionMode::AlwaysDeny);
    CHECK(most_restrictive(PermissionMode::AlwaysDeny, PermissionMode::AlwaysAllow) == PermissionMode::AlwaysDeny);

    SECTION("commutative and idempotent") {
        for (auto a : {PermissionMode::AlwaysDeny, PermissionMode::Ask, PermissionMode::AlwaysAllow}) {
            CHECK(most_restrictive(a, a) == a);
            for (auto b : {PermissionMode::AlwaysDeny, PermissionMode::Ask, PermissionMode::AlwaysAllow}) {
                CHECK(most_restrictive(a, b) == most_restrictive(b, a));
            }
        }
    }
}

TEST_CASE("out-of-workdir folds in rather than substituting", "[permission]") {
    // The bug this guards: if out_of_workdir REPLACED the tool's category, then
    // file_write=deny + out_of_workdir=allow would permit writing /etc/foo while
    // denying ./foo.cpp -- exactly inverted.
    PermissionPolicy policy;
    policy.file_write = PermissionMode::AlwaysDeny;
    policy.out_of_workdir = PermissionMode::AlwaysAllow;

    const auto inside = policy.mode_for("write", ToolCategory::FileWrite);
    const auto outside = most_restrictive(inside, policy.out_of_workdir);

    CHECK(inside == PermissionMode::AlwaysDeny);
    CHECK(outside == PermissionMode::AlwaysDeny);  // never more permissive than inside
}

TEST_CASE("network folds in rather than substituting", "[permission]") {
    // Same class of bug as out-of-workdir: an operator who sets execute=allow
    // for a sandboxed bash must not thereby also allow a network-capable
    // tool (S3's web_fetch) to reach arbitrary URLs.
    PermissionPolicy policy;
    policy.execute = PermissionMode::AlwaysAllow;
    policy.network = PermissionMode::AlwaysDeny;

    const auto base = policy.mode_for("bash", ToolCategory::Execute);
    const auto networked = most_restrictive(base, policy.network);

    CHECK(base == PermissionMode::AlwaysAllow);
    CHECK(networked == PermissionMode::AlwaysDeny);  // never more permissive than the category
}

TEST_CASE("apply_run_mode", "[permission]") {
    SECTION("Bypass allows everything, ignoring the base") {
        CHECK(apply_run_mode(PermissionMode::AlwaysDeny, PermissionRunMode::Bypass) == PermissionMode::AlwaysAllow);
    }
    SECTION("AllowOnly turns Ask into Deny so headless runs never block") {
        CHECK(apply_run_mode(PermissionMode::Ask, PermissionRunMode::AllowOnly) == PermissionMode::AlwaysDeny);
        CHECK(apply_run_mode(PermissionMode::AlwaysAllow, PermissionRunMode::AllowOnly) == PermissionMode::AlwaysAllow);
        CHECK(apply_run_mode(PermissionMode::AlwaysDeny, PermissionRunMode::AllowOnly) == PermissionMode::AlwaysDeny);
    }
    SECTION("Normal is identity") {
        for (auto m : {PermissionMode::AlwaysDeny, PermissionMode::Ask, PermissionMode::AlwaysAllow}) {
            CHECK(apply_run_mode(m, PermissionRunMode::Normal) == m);
        }
    }
}

TEST_CASE("a subagent can never escalate past its parent", "[permission][subagent]") {
    CHECK(clamp_run_mode(PermissionRunMode::Normal, PermissionRunMode::Bypass) == PermissionRunMode::Normal);
    CHECK(clamp_run_mode(PermissionRunMode::Normal, PermissionRunMode::AllowOnly) == PermissionRunMode::AllowOnly);
    CHECK(clamp_run_mode(PermissionRunMode::AllowOnly, PermissionRunMode::Bypass) == PermissionRunMode::AllowOnly);
}

TEST_CASE("per-tool override beats the category default", "[permission]") {
    PermissionPolicy policy;
    policy.execute = PermissionMode::Ask;
    policy.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;

    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysDeny);
    CHECK(policy.mode_for("other", ToolCategory::Execute) == PermissionMode::Ask);
}

TEST_CASE("a per-tool exact override still wins when no pattern matches",
          "[permission][pattern]") {
    // The 4-arg (subject-aware) overload's common path -- no regression from
    // folding the exact override against the pattern layer instead of
    // short-circuiting past it.
    PermissionPolicy policy;
    policy.read_only = PermissionMode::Ask;
    policy.per_tool_overrides["read"] = PermissionMode::AlwaysAllow;

    CHECK(policy.mode_for("read", ToolCategory::ReadOnly, std::string("/proj/main.cpp"),
                          /*subject_is_path=*/true) == PermissionMode::AlwaysAllow);
}

TEST_CASE("a matching deny pattern survives an AlwaysAllow exact override",
          "[permission][pattern][invariant]") {
    // THE BUG: per_tool_overrides used to return immediately, before the
    // pattern layer -- where the built-in dotenv denial lives
    // (default_pattern_rules) -- was ever consulted. An operator-set exact
    // AlwaysAllow (native permission_mode.overrides, or the opencode
    // tools:{read:true} alias, which is documented as a plain availability
    // toggle rather than a permission grant) silently bypassed the dotenv
    // protection for every path. most_restrictive folding closes it: the
    // pattern layer can still narrow what an exact override widened.
    PermissionPolicy policy;
    policy.read_only = PermissionMode::Ask;
    policy.per_tool_overrides["read"] = PermissionMode::AlwaysAllow;
    for (auto& [key, rules] : default_pattern_rules({"read", "write", "edit"})) {
        policy.pattern_overrides[key] = rules;
    }

    CHECK(policy.mode_for("read", ToolCategory::ReadOnly, std::string("/proj/.env"),
                          /*subject_is_path=*/true) == PermissionMode::AlwaysDeny);
    // A non-matching path is unaffected: the exact override still applies.
    CHECK(policy.mode_for("read", ToolCategory::ReadOnly, std::string("/proj/main.cpp"),
                          /*subject_is_path=*/true) == PermissionMode::AlwaysAllow);
}

TEST_CASE("MCP permission is server-scoped", "[permission][mcp]") {
    McpPermissionPolicy policy;
    policy.default_mode = PermissionMode::Ask;
    policy.per_server["trusted-search"] = PermissionMode::AlwaysAllow;

    CHECK(policy.mode_for("trusted-search") == PermissionMode::AlwaysAllow);
    CHECK(policy.mode_for("anything-else") == PermissionMode::Ask);
}
