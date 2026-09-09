#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::agent;
using minimal::tools::PermissionMode;

namespace {

AgentPresetFrontmatter Parse(const std::string& markdown) {
    return ParseAgentPresetFrontmatter(config::parse_frontmatter_markdown(markdown));
}

}  // namespace

TEST_CASE("ParseAgentPresetFrontmatter: minimal valid preset", "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "---\n"
        "You are a build agent.\n");

    CHECK(preset.name == "build");
    CHECK(preset.description.empty());
    CHECK(preset.model.empty());
    CHECK(preset.allowed_tools.empty());
    CHECK(preset.allowed_skills.empty());
    CHECK(preset.allowed_mcp_servers.empty());
    CHECK(preset.compression_prompt.empty());
    CHECK(preset.body == "You are a build agent.\n");
}

TEST_CASE("ParseAgentPresetFrontmatter: missing name throws", "[agent_preset_frontmatter]") {
    CHECK_THROWS_AS(Parse("---\ndescription: no name here\n---\nbody\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: no model is legal and means the default",
          "[agent_preset_frontmatter]") {
    // `provider` used to be required here. It no longer exists: a preset names
    // one model as <provider>/<model>, and omitting it defers to
    // settings.default_model -- which this layer cannot see, by design.
    auto preset = Parse("---\nname: build\n---\nbody\n");
    CHECK(preset.model.empty());
}

TEST_CASE("ParseAgentPresetFrontmatter: a malformed model ref throws here, not at the first turn",
          "[agent_preset_frontmatter]") {
    // Shape is decidable without a cross-file lookup, and catching it at load
    // names the preset. `qwen4` alone is the mistake someone carrying an old
    // preset forward will actually make.
    CHECK_THROWS_AS(Parse("---\nname: build\nmodel: qwen4\n---\nbody\n"), std::runtime_error);
    CHECK_THROWS_AS(Parse("---\nname: build\nmodel: local/\n---\nbody\n"), std::runtime_error);
    CHECK_THROWS_AS(Parse("---\nname: build\nmodel: a/b/c\n---\nbody\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: allowed_* lists decode", "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "description: Builds things\n"
        "model: openai-main/gpt-4.1\n"
        "allowed_tools: [read, write, bash]\n"
        "allowed_skills: [refactor]\n"
        "allowed_mcp_servers: [search]\n"
        "---\n"
        "body\n");

    CHECK(preset.description == "Builds things");
    CHECK(preset.model == "openai-main/gpt-4.1");
    CHECK(preset.allowed_tools == std::vector<std::string>{"read", "write", "bash"});
    CHECK(preset.allowed_skills == std::map<std::string, SkillMode>{{"refactor", SkillMode::Allow}});
    CHECK(preset.allowed_mcp_servers == std::vector<std::string>{"search"});
}

TEST_CASE("ParseAgentPresetFrontmatter: each permission_mode field is independently optional",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission_mode:\n"
        "  file_write: allow\n"
        "---\n"
        "body\n");

    REQUIRE(preset.permission_mode.file_write.has_value());
    CHECK(*preset.permission_mode.file_write == PermissionMode::AlwaysAllow);
    CHECK_FALSE(preset.permission_mode.read_only.has_value());
    CHECK_FALSE(preset.permission_mode.execute.has_value());
    CHECK_FALSE(preset.permission_mode.out_of_workdir.has_value());
    CHECK_FALSE(preset.permission_mode.network.has_value());
}

TEST_CASE("ParseAgentPresetFrontmatter: permission_mode.network decodes",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission_mode:\n"
        "  network: deny\n"
        "---\n"
        "body\n");

    REQUIRE(preset.permission_mode.network.has_value());
    CHECK(*preset.permission_mode.network == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParseAgentPresetFrontmatter: permission_mode.overrides decodes into per_tool_overrides",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission_mode:\n"
        "  overrides:\n"
        "    bash: deny\n"
        "---\n"
        "body\n");

    REQUIRE(preset.permission_mode.per_tool_overrides.count("bash") == 1);
    CHECK(preset.permission_mode.per_tool_overrides.at("bash") == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParseAgentPresetFrontmatter: empty-string override key throws", "[agent_preset_frontmatter]") {
    CHECK_THROWS_AS(Parse(
                        "---\n"
                        "name: build\n"
                                        "permission_mode:\n"
                        "  overrides:\n"
                        "    \"\": deny\n"
                        "---\n"
                        "body\n"),
                    std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: mcp_permissions decodes", "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "mcp_permissions:\n"
        "  search: allow\n"
        "---\n"
        "body\n");

    REQUIRE(preset.mcp_permissions.count("search") == 1);
    CHECK(preset.mcp_permissions.at("search") == PermissionMode::AlwaysAllow);
}

TEST_CASE("ParseAgentPresetFrontmatter: malformed permission mode string throws", "[agent_preset_frontmatter]") {
    CHECK_THROWS_AS(Parse(
                        "---\n"
                        "name: build\n"
                                        "permission_mode:\n"
                        "  execute: maybe\n"
                        "---\n"
                        "body\n"),
                    std::runtime_error);
}

// --- opencode-compat aliases ----------------------------------------------------

TEST_CASE("ParseAgentPresetFrontmatter: the permission alias maps onto categories",
          "[agent_preset_frontmatter][compat]") {
    auto preset = Parse(
        "---\n"
        "name: review\n"
        "mode: subagent\n"
        "permission:\n"
        "  edit: deny\n"
        "  bash: ask\n"
        "  webfetch: deny\n"
        "  external_directory: allow\n"
        "  read: allow\n"
        "---\n"
        "Only analyze code.\n");

    REQUIRE(preset.permission_mode.file_write.has_value());
    CHECK(*preset.permission_mode.file_write == PermissionMode::AlwaysDeny);
    REQUIRE(preset.permission_mode.execute.has_value());
    CHECK(*preset.permission_mode.execute == PermissionMode::Ask);
    REQUIRE(preset.permission_mode.network.has_value());
    CHECK(*preset.permission_mode.network == PermissionMode::AlwaysDeny);
    REQUIRE(preset.permission_mode.out_of_workdir.has_value());
    CHECK(*preset.permission_mode.out_of_workdir == PermissionMode::AlwaysAllow);
    REQUIRE(preset.permission_mode.read_only.has_value());
    CHECK(*preset.permission_mode.read_only == PermissionMode::AlwaysAllow);
}

TEST_CASE("ParseAgentPresetFrontmatter: a pattern-map permission value becomes rules, not a "
          "collapsed scalar",
          "[agent_preset_frontmatter][patterns][compat]") {
    // The pattern layer is real now: every entry lands as a rule matched
    // against the command at resolve time, so the old most-restrictive
    // collapse (which destroyed the author's granularity) no longer happens.
    auto catchall = Parse(
        "---\n"
        "name: review\n"
        "permission:\n"
        "  bash:\n"
        "    \"*\": ask\n"
        "    \"git *\": allow\n"
        "---\n"
        "body\n");
    CHECK_FALSE(catchall.permission_mode.execute.has_value());
    REQUIRE(catchall.permission_mode.pattern_overrides.count("bash") == 1);
    const auto& catchall_rules = catchall.permission_mode.pattern_overrides.at("bash");
    REQUIRE(catchall_rules.size() == 2);
    CHECK(catchall_rules[0].glob == "*");
    CHECK(catchall_rules[0].mode == PermissionMode::Ask);
    CHECK(catchall_rules[1].glob == "git *");
    CHECK(catchall_rules[1].mode == PermissionMode::AlwaysAllow);

    auto restrictive = Parse(
        "---\n"
        "name: review\n"
        "permission:\n"
        "  bash:\n"
        "    \"git *\": allow\n"
        "    \"rm *\": deny\n"
        "---\n"
        "body\n");
    CHECK_FALSE(restrictive.permission_mode.execute.has_value());
    REQUIRE(restrictive.permission_mode.pattern_overrides.count("bash") == 1);
    // The deny survives as a rule -- the old collapse-to-deny is now exact.
    bool rm_is_deny = false;
    for (const auto& rule : restrictive.permission_mode.pattern_overrides.at("bash")) {
        if (rule.glob == "rm *") rm_is_deny = rule.mode == PermissionMode::AlwaysDeny;
    }
    CHECK(rm_is_deny);
}

TEST_CASE("ParseAgentPresetFrontmatter: a native permission_mode wins over the alias",
          "[agent_preset_frontmatter][compat]") {
    auto preset = Parse(
        "---\n"
        "name: review\n"
        "permission_mode:\n"
        "  file_write: allow\n"
        "permission:\n"
        "  edit: deny\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.file_write.has_value());
    CHECK(*preset.permission_mode.file_write == PermissionMode::AlwaysAllow);
}

TEST_CASE("ParseAgentPresetFrontmatter: the deprecated tools map folds into overrides",
          "[agent_preset_frontmatter][compat]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "tools:\n"
        "  write: false\n"
        "  read: true\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.per_tool_overrides.count("write") == 1);
    CHECK(preset.permission_mode.per_tool_overrides.at("write") == PermissionMode::AlwaysDeny);
    CHECK(preset.permission_mode.per_tool_overrides.at("read") == PermissionMode::AlwaysAllow);
}

TEST_CASE("ParseAgentPresetFrontmatter: a native permission_mode override wins over the "
          "tools alias",
          "[agent_preset_frontmatter][compat]") {
    // The stated contract: native beats alias. Applying the tools alias AFTER
    // the permission_mode block used to invert this -- tools.write re-allowed
    // what the native file denied.
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission_mode:\n"
        "  overrides:\n"
        "    write: deny\n"
        "tools:\n"
        "  write: true\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.per_tool_overrides.count("write") == 1);
    CHECK(preset.permission_mode.per_tool_overrides.at("write") == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParseAgentPresetFrontmatter: alias keys collapsing onto one category keep the "
          "most restrictive",
          "[agent_preset_frontmatter][compat]") {
    // read/glob/grep/list all feed read_only, applied in YAML document order;
    // a later permissive key must not overwrite an earlier deny and WIDEN what
    // the author asked for.
    auto preset = Parse(
        "---\n"
        "name: review\n"
        "permission:\n"
        "  read: deny\n"
        "  glob: allow\n"
        "  grep: deny\n"
        "  list: ask\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.read_only.has_value());
    CHECK(*preset.permission_mode.read_only == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParseAgentPresetFrontmatter: mode decodes and validates",
          "[agent_preset_frontmatter][compat]") {
    CHECK(Parse("---\nname: a\nmode: subagent\n---\nb\n").mode == AgentMode::Subagent);
    CHECK(Parse("---\nname: a\nmode: primary\n---\nb\n").mode == AgentMode::Primary);
    CHECK(Parse("---\nname: a\n---\nb\n").mode == AgentMode::All);
    CHECK_THROWS_AS(Parse("---\nname: a\nmode: sideways\n---\nb\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: disable, hidden, color and steps decode",
          "[agent_preset_frontmatter][compat]") {
    auto preset = Parse(
        "---\n"
        "name: a\n"
        "disable: false\n"
        "hidden: true\n"
        "color: \"#ff6b6b\"\n"
        "steps: 12\n"
        "---\n"
        "body\n");
    CHECK_FALSE(preset.disable);
    CHECK(preset.hidden);
    CHECK(preset.color == "#ff6b6b");
    REQUIRE(preset.steps.has_value());
    CHECK(*preset.steps == 12);

    CHECK_THROWS_AS(Parse("---\nname: a\nhidden: yes-please\n---\nb\n"), std::runtime_error);
    CHECK_THROWS_AS(Parse("---\nname: a\nsteps: -3\n---\nb\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: unknown keys become provider parameter passthrough",
          "[agent_preset_frontmatter][compat]") {
    auto preset = Parse(
        "---\n"
        "name: deep-thinker\n"
        "model: openai/gpt-5\n"
        "temperature: 0.1\n"
        "top_p: 0.9\n"
        "reasoningEffort: high\n"
        "---\n"
        "body\n");
    CHECK(preset.extra_parameters["temperature"] == 0.1);
    CHECK(preset.extra_parameters["top_p"] == 0.9);
    CHECK(preset.extra_parameters["reasoningEffort"] == "high");
}

// --- tool_config -----------------------------------------------------------------

TEST_CASE("ParseAgentPresetFrontmatter: tool_config is a structural namespace map",
          "[agent_preset_frontmatter][tool_config]") {
    auto preset = Parse(
        "---\n"
        "name: a\n"
        "tool_config:\n"
        "  python:\n"
        "    venv: .venv\n"
        "    packages: [impacket, pefile]\n"
        "  other-plugin:\n"
        "    nested:\n"
        "      deep: true\n"
        "---\n"
        "body\n");
    REQUIRE(preset.tool_config.contains("python"));
    CHECK(preset.tool_config["python"]["venv"] == ".venv");
    REQUIRE(preset.tool_config["python"]["packages"].is_array());
    CHECK(preset.tool_config["python"]["packages"][0] == "impacket");
    CHECK(preset.tool_config["other-plugin"]["nested"]["deep"] == true);
}

TEST_CASE("ParseAgentPresetFrontmatter: absent tool_config is an empty object",
          "[agent_preset_frontmatter][tool_config]") {
    auto preset = Parse("---\nname: a\n---\nbody\n");
    CHECK(preset.tool_config.is_object());
    CHECK(preset.tool_config.empty());
}

TEST_CASE("ParseAgentPresetFrontmatter: tool_config never leaks into extra_parameters",
          "[agent_preset_frontmatter][tool_config]") {
    auto preset = Parse(
        "---\n"
        "name: a\n"
        "tool_config:\n"
        "  python:\n"
        "    venv: .venv\n"
        "---\n"
        "body\n");
    CHECK(preset.extra_parameters.empty());
}

TEST_CASE("ParseAgentPresetFrontmatter: malformed tool_config throws",
          "[agent_preset_frontmatter][tool_config]") {
    // A scalar is not a namespace map.
    CHECK_THROWS_AS(Parse("---\nname: a\ntool_config: .venv\n---\nb\n"), std::runtime_error);
    // A sequence is not a namespace map either.
    CHECK_THROWS_AS(Parse("---\nname: a\ntool_config: [a, b]\n---\nb\n"), std::runtime_error);
    // An empty-string namespace key would be unreachable by any tool.
    CHECK_THROWS_AS(Parse("---\nname: a\ntool_config:\n  \"\": {}\n---\nb\n"), std::runtime_error);
}

// --- allowed_skills modes -------------------------------------------------------

TEST_CASE("ParseAgentPresetFrontmatter: allowed_skills accepts the list and the map form",
          "[agent_preset_frontmatter][compat]") {
    auto list = Parse("---\nname: a\nallowed_skills: [git-release]\n---\nb\n");
    CHECK(list.allowed_skills ==
          std::map<std::string, SkillMode>{{"git-release", SkillMode::Allow}});

    auto map = Parse(
        "---\n"
        "name: a\n"
        "allowed_skills:\n"
        "  \"*\": allow\n"
        "  git-release: preload\n"
        "  internal-docs: ask\n"
        "  experimental-x: deny\n"
        "---\n"
        "b\n");
    CHECK(map.allowed_skills ==
          (std::map<std::string, SkillMode>{
              {"*", SkillMode::Allow},
              {"git-release", SkillMode::Preload},
              {"internal-docs", SkillMode::Ask},
              {"experimental-x", SkillMode::Deny},
          }));
}

TEST_CASE("ParseAgentPresetFrontmatter: an invalid allowed_skills mode throws",
          "[agent_preset_frontmatter][compat]") {
    CHECK_THROWS_AS(Parse("---\nname: a\nallowed_skills:\n  x: sometimes\n---\nb\n"),
                    std::runtime_error);
    CHECK_THROWS_AS(Parse("---\nname: a\nallowed_skills: 42\n---\nb\n"), std::runtime_error);
}

// --- pattern rules -----------------------------------------------------------------

TEST_CASE("ParseAgentPresetFrontmatter: opencode permission maps become pattern rules",
          "[agent_preset_frontmatter][patterns][compat]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission:\n"
        "  bash:\n"
        "    \"git *\": allow\n"
        "    \"git push --force *\": ask\n"
        "  read:\n"
        "    \"**/.env\": deny\n"
        "---\n"
        "body\n");

    REQUIRE(preset.permission_mode.pattern_overrides.count("bash") == 1);
    const auto& bash_rules = preset.permission_mode.pattern_overrides.at("bash");
    REQUIRE(bash_rules.size() == 2);
    CHECK(bash_rules[0].glob == "git *");
    CHECK(bash_rules[0].mode == PermissionMode::AlwaysAllow);
    CHECK(bash_rules[1].mode == PermissionMode::Ask);
    REQUIRE(preset.permission_mode.pattern_overrides.count("read") == 1);
    CHECK(preset.permission_mode.pattern_overrides.at("read")[0].mode ==
          PermissionMode::AlwaysDeny);
    // No scalar collapsed onto the categories: the map was the pattern layer.
    CHECK_FALSE(preset.permission_mode.execute.has_value());
}

TEST_CASE("ParseAgentPresetFrontmatter: scalar alias values still map to categories",
          "[agent_preset_frontmatter][patterns][compat]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission:\n"
        "  bash: ask\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.execute.has_value());
    CHECK(*preset.permission_mode.execute == PermissionMode::Ask);
    CHECK(preset.permission_mode.pattern_overrides.empty());
}

TEST_CASE("ParseAgentPresetFrontmatter: external_directory map becomes out_of_workdir patterns",
          "[agent_preset_frontmatter][patterns][compat]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission:\n"
        "  external_directory:\n"
        "    \"/tmp/builds/**\": allow\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.out_of_workdir_patterns.size() == 1);
    CHECK(preset.permission_mode.out_of_workdir_patterns[0].glob == "/tmp/builds/**");
    CHECK(preset.permission_mode.out_of_workdir_patterns[0].mode == PermissionMode::AlwaysAllow);
    CHECK_FALSE(preset.permission_mode.out_of_workdir.has_value());
}

TEST_CASE("ParseAgentPresetFrontmatter: native permission_mode.patterns decode",
          "[agent_preset_frontmatter][patterns]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission_mode:\n"
        "  patterns:\n"
        "    bash:\n"
        "      \"git *\": allow\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.pattern_overrides.count("bash") == 1);
    CHECK(preset.permission_mode.pattern_overrides.at("bash")[0].glob == "git *");
    CHECK(preset.permission_mode.pattern_overrides.at("bash")[0].mode ==
          PermissionMode::AlwaysAllow);
}

TEST_CASE("LoadAgentPresetFrontmatter: an oversized file is refused before the read runs",
          "[agent_preset_frontmatter][invariant]") {
    // agents/*.md is UNTRUSTED, project-local content (walked ahead of the
    // operator's own config_dir -- see AgentPresetRegistry::ResolveUncached),
    // and yaml-cpp's recursive-descent parser has no built-in size or depth
    // limit. A file over the cap must be refused by NAME (not silently
    // truncated, not attempted) before the read even opens the file.
    testing::TempDir dir;
    const auto path = dir.path() / "oversized.md";
    {
        std::ofstream out(path, std::ios::binary);
        out << "---\nname: build\n---\n";
        // Comfortably over the 1 MiB cap.
        out << std::string(2 << 20, 'x');
    }
    CHECK_THROWS_AS(LoadAgentPresetFrontmatter(path), std::runtime_error);
}

TEST_CASE("LoadAgentPresetFrontmatter: a normally-sized file is unaffected by the cap",
          "[agent_preset_frontmatter]") {
    testing::TempDir dir;
    const auto path = dir.path() / "normal.md";
    {
        std::ofstream out(path);
        out << "---\nname: build\n---\nbody\n";
    }
    const auto preset = LoadAgentPresetFrontmatter(path);
    CHECK(preset.name == "build");
}
