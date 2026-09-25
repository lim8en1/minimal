#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/permission_resolution.hpp"
#include "minimal/mcp/mcp_permission_model.hpp"

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

TEST_CASE("BuildPermissionPolicy: a preset category cannot widen a global per-tool rule",
          "[permission_resolution]") {
    // A blanket preset category and a specific global rule that both speak
    // fold most-restrictively: `bash: deny` stays deny under `execute: allow`.
    PermissionOverrideLayer preset;
    preset.execute = PermissionMode::AlwaysAllow;  // preset's blanket category default
    PermissionOverrideLayer global;
    global.per_tool_overrides["bash"] = PermissionMode::AlwaysDeny;

    auto policy = BuildPermissionPolicy(preset, global);
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysDeny);
    // An unrelated tool in the same category still gets the preset's category default.
    CHECK(policy.mode_for("other", ToolCategory::Execute) == PermissionMode::AlwaysAllow);
}

TEST_CASE("BuildPermissionPolicy: a global per-tool allow cannot widen a preset's category",
          "[permission_resolution][regression]") {
    // THE REGRESSION: the flat merge put every per-tool rule above every
    // category, so the headless recipe `bash: allow` turned plan's
    // `execute: deny` into allow.
    PermissionOverrideLayer plan;
    plan.execute = PermissionMode::AlwaysDeny;
    PermissionOverrideLayer global;
    global.per_tool_overrides["bash"] = PermissionMode::AlwaysAllow;

    auto policy = BuildPermissionPolicy(plan, global);
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysDeny);
    // Without a preset opinion on the category, the global rule stands.
    auto build = BuildPermissionPolicy({}, global);
    CHECK(build.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysAllow);
    // And a preset per-tool rule is the most specific statement: it wins.
    plan.per_tool_overrides["bash"] = PermissionMode::Ask;
    auto pinned = BuildPermissionPolicy(plan, global);
    CHECK(pinned.mode_for("bash", ToolCategory::Execute) == PermissionMode::Ask);
}

TEST_CASE("BuildPermissionPolicy: a preset's pattern carve-out still works under its category",
          "[permission_resolution][patterns]") {
    // plan.md's shape: execute deny + allow patterns. A matching pattern
    // replaces the category; the global bash allow still does not reach
    // commands no pattern covers.
    PermissionOverrideLayer plan;
    plan.execute = PermissionMode::AlwaysDeny;
    plan.pattern_overrides["bash"].push_back({"git status", PermissionMode::AlwaysAllow});
    PermissionOverrideLayer global;
    global.per_tool_overrides["bash"] = PermissionMode::AlwaysAllow;
    auto policy = BuildPermissionPolicy(plan, global);

    minimal::tools::PatternSubjects git;
    git.whole = "git status";
    git.stages = {"git status"};
    CHECK(policy.mode_for("bash", ToolCategory::Execute, git, false) ==
          PermissionMode::AlwaysAllow);
    minimal::tools::PatternSubjects rm;
    rm.whole = "rm -rf x";
    rm.stages = {"rm -rf x"};
    CHECK(policy.mode_for("bash", ToolCategory::Execute, rm, false) ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("BuildPermissionPolicy: an untrusted preset's narrowing holds against global patterns",
          "[permission_resolution][patterns]") {
    PermissionOverrideLayer project;
    project.execute = PermissionMode::AlwaysDeny;
    project.per_tool_overrides["grep"] = PermissionMode::AlwaysAllow;  // ignored as a widening
    PermissionOverrideLayer global;
    global.read_only = PermissionMode::Ask;
    global.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});
    auto policy = BuildPermissionPolicy(project, global, /*preset_is_trusted=*/false);

    minimal::tools::PatternSubjects git;
    git.whole = "git log";
    git.stages = {"git log"};
    CHECK(policy.mode_for("bash", ToolCategory::Execute, git, false) ==
          PermissionMode::AlwaysDeny);
    CHECK(policy.mode_for("grep", ToolCategory::ReadOnly) == PermissionMode::Ask);
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

TEST_CASE("BuildMcpPolicy: trusted preset per-server wins", "[permission_resolution]") {
    auto policy = minimal::mcp::BuildMcpPolicy({{"search", PermissionMode::AlwaysAllow}},
                                           PermissionMode::AlwaysDeny, true);
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysAllow);
}

TEST_CASE("BuildMcpPolicy: unset server falls to global default", "[permission_resolution]") {
    auto policy = minimal::mcp::BuildMcpPolicy({}, PermissionMode::AlwaysDeny, true);
    CHECK(policy.mode_for("unconfigured") == PermissionMode::AlwaysDeny);
}

TEST_CASE("BuildMcpPolicy: an untrusted preset may not widen the global default",
          "[permission_resolution]") {
    // Allow under a Deny floor folds to Deny; Allow under Ask folds to Ask.
    // The clamp is what stops a repo-shipped preset from auto-approving MCP.
    auto policy = minimal::mcp::BuildMcpPolicy({{"search", PermissionMode::AlwaysAllow}},
                                           PermissionMode::AlwaysDeny, false);
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysDeny);

    auto ask_floor = minimal::mcp::BuildMcpPolicy({{"search", PermissionMode::AlwaysAllow}},
                                              PermissionMode::Ask, false);
    CHECK(ask_floor.mode_for("search") == PermissionMode::Ask);
}

TEST_CASE("BuildMcpPolicy: an untrusted preset may still narrow", "[permission_resolution]") {
    // Deny under an Allow default stays Deny: narrowing is the whole point of
    // letting an untrusted preset speak at all.
    auto policy = minimal::mcp::BuildMcpPolicy({{"search", PermissionMode::AlwaysDeny}},
                                           PermissionMode::AlwaysAllow, false);
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysDeny);
}

// --- pattern rules -----------------------------------------------------------------

TEST_CASE("BuildPermissionPolicy: the built-in dotenv defaults merge per glob",
          "[permission_resolution][patterns]") {
    // Production passes ToolRegistry::DotenvProtectedPermissionKeys() (see
    // main.cpp) -- {"read", "write", "edit"} pins today's tools-fs set at the
    // test, structurally identical to what the live registry resolves to.
    const std::vector<std::string> dotenv_protected = {"read", "write", "edit"};
    auto policy = BuildPermissionPolicy({}, {}, /*preset_is_trusted=*/true, dotenv_protected);
    REQUIRE(policy.pattern_overrides.count("read") == 1);
    REQUIRE(policy.pattern_overrides.count("write") == 1);
    REQUIRE(policy.pattern_overrides.count("edit") == 1);
    CHECK(policy.pattern_overrides.count("bash") == 0);

    // An UNRELATED operator rule for a tool keeps its defaults: it used to
    // replace them wholesale, silently re-opening .env for that tool.
    PermissionOverrideLayer global;
    global.pattern_overrides["read"].push_back({"**/scratch", PermissionMode::AlwaysAllow});
    auto customized = BuildPermissionPolicy({}, global, /*preset_is_trusted=*/true, dotenv_protected);
    const auto& read_rules = customized.pattern_overrides.at("read");
    REQUIRE(read_rules.size() == 3);
    CHECK(read_rules[0].glob == "**/scratch");
    CHECK(customized.mode_for("read", ToolCategory::ReadOnly,
                              std::optional<std::string>("/p/.env"), true) ==
          PermissionMode::AlwaysDeny);

    // A rule on the SAME glob replaces that default -- the documented way to
    // lift it -- while the sibling default stays.
    PermissionOverrideLayer lifting;
    lifting.pattern_overrides["read"].push_back({"**/.env", PermissionMode::AlwaysAllow});
    auto lifted = BuildPermissionPolicy({}, lifting, /*preset_is_trusted=*/true, dotenv_protected);
    CHECK(lifted.mode_for("read", ToolCategory::ReadOnly, std::optional<std::string>("/p/.env"),
                          true) == PermissionMode::AlwaysAllow);
    CHECK(lifted.mode_for("read", ToolCategory::ReadOnly,
                          std::optional<std::string>("/p/.env.local"), true) ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("BuildPermissionPolicy: dotenv defaults apply to any tool that opts in, not just "
          "read/write/edit",
          "[permission_resolution][patterns]") {
    // A renamed or third-party file tool works exactly like tools-fs's
    // bundled ones (ARCHITECTURE.md, "Core vs. plugin surface" -- the
    // read/write/edit "not structurally privileged" claim, made true).
    auto policy = BuildPermissionPolicy({}, {}, /*preset_is_trusted=*/true, {"my_renamed_tool"});
    REQUIRE(policy.pattern_overrides.count("my_renamed_tool") == 1);
    const auto& rules = policy.pattern_overrides.at("my_renamed_tool");
    REQUIRE(rules.size() == 2);
    CHECK(rules[0].glob == "**/.env");
    CHECK(rules[0].mode == PermissionMode::AlwaysDeny);
    CHECK(rules[1].glob == "**/.env.*");
    CHECK(policy.pattern_overrides.count("read") == 0);
}

TEST_CASE("BuildPermissionPolicy: trusted preset patterns replace global per glob",
          "[permission_resolution][patterns]") {
    PermissionOverrideLayer global;
    global.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});
    global.pattern_overrides["bash"].push_back({"rm *", PermissionMode::AlwaysDeny});
    PermissionOverrideLayer preset;
    preset.pattern_overrides["bash"].push_back({"git *", PermissionMode::Ask});

    auto policy = BuildPermissionPolicy(preset, global, /*preset_is_trusted=*/true);
    const auto& rules = policy.pattern_overrides["bash"];
    REQUIRE(rules.size() == 2);
    bool git_is_ask = false, rm_is_deny = false;
    for (const auto& rule : rules) {
        if (rule.glob == "git *") git_is_ask = rule.mode == PermissionMode::Ask;
        if (rule.glob == "rm *") rm_is_deny = rule.mode == PermissionMode::AlwaysDeny;
    }
    CHECK(git_is_ask);   // the preset replaced the global entry for the same glob
    CHECK(rm_is_deny);   // ...and left the distinct global rule alone
}

TEST_CASE("BuildPermissionPolicy: an untrusted preset contributes no pattern rules",
          "[permission_resolution][patterns][invariant]") {
    // A pattern's effect depends on what it matches at resolve time, so "it
    // only narrowed" cannot be established statically -- a repo-shipped
    // {"git *": "allow"} would be a widening the trust clamp never sees.
    // The only safe rule: project-local presets contribute nothing.
    PermissionOverrideLayer global;
    global.pattern_overrides["bash"].push_back({"git status", PermissionMode::Ask});
    PermissionOverrideLayer preset;
    preset.pattern_overrides["bash"].push_back({"git *", PermissionMode::AlwaysAllow});

    auto policy = BuildPermissionPolicy(preset, global, /*preset_is_trusted=*/false);
    REQUIRE(policy.pattern_overrides.count("bash") == 1);
    CHECK(policy.pattern_overrides["bash"].size() == 1);
    CHECK(policy.pattern_overrides["bash"][0].glob == "git status");
}

TEST_CASE("BuildPermissionPolicy: external_directory patterns merge per glob",
          "[permission_resolution][patterns]") {
    PermissionOverrideLayer global;
    global.out_of_workdir_patterns.push_back({"/tmp/**", PermissionMode::Ask});
    PermissionOverrideLayer preset;
    preset.out_of_workdir_patterns.push_back({"/var/**", PermissionMode::AlwaysAllow});

    auto policy = BuildPermissionPolicy(preset, global, /*preset_is_trusted=*/true);
    REQUIRE(policy.out_of_workdir_patterns.size() == 2);
}

TEST_CASE("BuildPermissionPolicy: pattern_precedence resolves preset over global, never untrusted",
          "[permission_resolution][patterns]") {
    using minimal::tools::PatternPrecedence;
    PermissionOverrideLayer global;
    CHECK(BuildPermissionPolicy({}, global).pattern_precedence == PatternPrecedence::Restrictive);
    global.pattern_precedence = PatternPrecedence::Specific;
    CHECK(BuildPermissionPolicy({}, global).pattern_precedence == PatternPrecedence::Specific);

    PermissionOverrideLayer preset;
    preset.pattern_precedence = PatternPrecedence::Restrictive;
    CHECK(BuildPermissionPolicy(preset, global).pattern_precedence ==
          PatternPrecedence::Restrictive);

    PermissionOverrideLayer project;  // untrusted: may not choose Specific
    project.pattern_precedence = PatternPrecedence::Specific;
    CHECK(BuildPermissionPolicy(project, {}, /*preset_is_trusted=*/false).pattern_precedence ==
          PatternPrecedence::Restrictive);
}
