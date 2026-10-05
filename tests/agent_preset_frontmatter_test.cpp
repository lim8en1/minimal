#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::agent;
using minimal::tools::PermissionMode;

namespace {

// Pins today's bundled tool vocabulary at the test -- production passes
// ToolRegistry::PermissionKeys() (see agent_preset_registry.cpp), not a
// literal list.
// The web tools declare their opencode spellings and take no pattern rules
// (Tool::config_aliases / accepts_pattern_rules), exactly as tools-web does.
ToolVocabulary BundledTools() {
    ToolVocabulary tools(std::vector<std::string>{"bash", "read", "write", "edit", "glob", "grep",
                                                  "web_fetch", "web_search"});
    tools.config_aliases = {{"webfetch", "web_fetch"}, {"websearch", "web_search"}};
    tools.no_pattern_rules = {"web_fetch", "web_search"};
    return tools;
}

AgentPresetFrontmatter Parse(const std::string& markdown) {
    return ParseAgentPresetFrontmatter(config::parse_frontmatter_markdown(markdown),
                                       BundledTools());
}

AgentPresetFrontmatter ParseWithTools(const std::string& markdown,
                                      const std::vector<std::string>& known_tool_permission_keys) {
    return ParseAgentPresetFrontmatter(config::parse_frontmatter_markdown(markdown),
                                       known_tool_permission_keys);
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
    CHECK(preset.tool_sources.empty());
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
    CHECK_THROWS_AS(Parse("---\nname: build\nmodel: /qwen4\n---\nbody\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: a discovered model id may itself contain a slash",
          "[agent_preset_frontmatter]") {
    // SplitModelRef, not ParseModelRef (see its comment in model_ref.hpp): a
    // preset naming an OpenRouter-shaped model under provider "openrouter"
    // must not be rejected here just because it has two slashes -- the FIRST
    // slash is the provider boundary, same as --model/`/model`.
    auto preset = Parse("---\nname: build\nmodel: openrouter/anthropic/claude-opus-5\n---\nbody\n");
    CHECK(preset.model == "openrouter/anthropic/claude-opus-5");
}

TEST_CASE("ParseAgentPresetFrontmatter: allowed_* lists decode", "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "description: Builds things\n"
        "model: openai-main/gpt-4.1\n"
        "allowed_tools: [read, write, bash]\n"
        "tool_sources:\n"
        "  mcp: [search]\n"
        "---\n"
        "body\n");

    CHECK(preset.description == "Builds things");
    CHECK(preset.model == "openai-main/gpt-4.1");
    CHECK(preset.allowed_tools == std::vector<std::string>{"read", "write", "bash"});
    CHECK(preset.tool_sources.at("mcp") == std::vector<std::string>{"search"});
}

TEST_CASE("ParseAgentPresetFrontmatter: allowed_subagents is absent-vs-list, not just a list",
          "[agent_preset_frontmatter]") {
    // The empty list is SEMANTIC (no spawns at all), so absent must stay
    // distinguishable from present-but-empty -- which is why this field cannot
    // share the OptionalStringList lowering, whose empty result is ambiguous.
    CHECK_FALSE(Parse("---\nname: build\n---\nbody\n").allowed_subagents.has_value());

    const auto listed = Parse(
        "---\nname: build\nallowed_subagents: [researcher, \"*\"]\n---\nbody\n");
    REQUIRE(listed.allowed_subagents.has_value());
    CHECK(*listed.allowed_subagents == std::vector<std::string>{"researcher", "*"});

    const auto empty = Parse("---\nname: build\nallowed_subagents: []\n---\nbody\n");
    REQUIRE(empty.allowed_subagents.has_value());
    CHECK(empty.allowed_subagents->empty());
}

TEST_CASE("ParseAgentPresetFrontmatter: allowed_subagents shape errors name the field",
          "[agent_preset_frontmatter]") {
    CHECK_THROWS_AS(
        Parse("---\nname: build\nallowed_subagents: researcher\n---\nbody\n"),
        std::runtime_error);  // scalar, not a list
    CHECK_THROWS_AS(
        Parse("---\nname: build\nallowed_subagents:\n  - \"\"\n---\nbody\n"),
        std::runtime_error);  // an empty entry would match nothing or everything
}

TEST_CASE("ParseAgentPresetFrontmatter: allowed_subagents is structural, not provider passthrough",
          "[agent_preset_frontmatter]") {
    // Every unrecognized key lands in extra_parameters and rides to the
    // provider as a request parameter; if allowed_subagents were missing from
    // the known-key table, `allowed_subagents: [x]` would silently become an
    // API argument.
    const auto preset = Parse(
        "---\nname: build\nallowed_subagents: [researcher]\n---\nbody\n");
    CHECK(preset.extra_parameters.empty());
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

TEST_CASE("ParseAgentPresetFrontmatter: plugin_permissions carries slices verbatim",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "plugin_permissions:\n"
        "  tools-web:\n"
        "    hosts:\n"
        "      \"*.internal\": deny\n"
        "---\n"
        "body\n");
    CHECK(preset.plugin_permissions["tools-web"]["hosts"]["*.internal"] == "deny");
    CHECK(preset.extra_parameters.empty());

    CHECK_THROWS(Parse("---\nname: build\nplugin_permissions: [a]\n---\nbody\n"));
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

    // edit governs every file-modifying tool in opencode: the category.
    REQUIRE(preset.permission_mode.file_write.has_value());
    CHECK(*preset.permission_mode.file_write == PermissionMode::AlwaysDeny);
    REQUIRE(preset.permission_mode.out_of_workdir.has_value());
    CHECK(*preset.permission_mode.out_of_workdir == PermissionMode::AlwaysAllow);
    // Single-tool keys govern that tool only -- never its whole category.
    const auto& tools = preset.permission_mode.per_tool_overrides;
    CHECK(tools.at("bash") == PermissionMode::Ask);
    CHECK(tools.at("web_fetch") == PermissionMode::AlwaysDeny);
    CHECK(tools.at("read") == PermissionMode::AlwaysAllow);
    CHECK_FALSE(preset.permission_mode.execute.has_value());
    CHECK_FALSE(preset.permission_mode.network.has_value());
    CHECK_FALSE(preset.permission_mode.read_only.has_value());
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

TEST_CASE("ParseAgentPresetFrontmatter: a tools key is neither a permission nor a provider "
          "parameter",
          "[agent_preset_frontmatter]") {
    // opencode's old `tools: {name: bool}` map is not read; and `tools` names
    // a request field the harness owns, so it never passes through either.
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "tools:\n"
        "  write: false\n"
        "---\n"
        "body\n");
    CHECK(preset.permission_mode.per_tool_overrides.empty());
    CHECK_FALSE(preset.extra_parameters.contains("tools"));
}

TEST_CASE("ParseAgentPresetFrontmatter: a permission key resolves through the live tool "
          "vocabulary, not a fixed list",
          "[agent_preset_frontmatter]") {
    ToolVocabulary tools(std::vector<std::string>{"remote_exec"});
    tools.config_aliases = {{"rexec", "remote_exec"}};
    const auto preset = ParseAgentPresetFrontmatter(
        config::parse_frontmatter_markdown("---\nname: build\npermission:\n  rexec: deny\n"
                                           "  bash: allow\n---\nbody\n"),
        tools);
    // The declared alias reaches its tool; `bash` is no tool here, so it is
    // named and ignored rather than assumed.
    CHECK(preset.permission_mode.per_tool_overrides.at("remote_exec") == PermissionMode::AlwaysDeny);
    CHECK(preset.permission_mode.per_tool_overrides.count("bash") == 0);
}

TEST_CASE("ParseAgentPresetFrontmatter: alias keys collapsing onto one category keep the "
          "most restrictive",
          "[agent_preset_frontmatter][compat]") {
    // edit/write both feed file_write, applied in YAML document order; a later
    // permissive key must not overwrite an earlier deny and WIDEN what the
    // author asked for.
    auto preset = Parse(
        "---\n"
        "name: review\n"
        "permission:\n"
        "  edit: deny\n"
        "  write: allow\n"
        "---\n"
        "body\n");
    REQUIRE(preset.permission_mode.file_write.has_value());
    CHECK(*preset.permission_mode.file_write == PermissionMode::AlwaysDeny);
}

TEST_CASE("ParseAgentPresetFrontmatter: read-side alias keys stay per tool",
          "[agent_preset_frontmatter][compat]") {
    // `read: deny` once denied the whole read_only category -- grep, glob,
    // ask_user, skills. `list` names no minimal tool and is ignored (warned).
    auto preset = Parse(
        "---\n"
        "name: review\n"
        "permission:\n"
        "  read: deny\n"
        "  glob: allow\n"
        "  list: deny\n"
        "---\n"
        "body\n");
    CHECK_FALSE(preset.permission_mode.read_only.has_value());
    CHECK(preset.permission_mode.per_tool_overrides.at("read") == PermissionMode::AlwaysDeny);
    CHECK(preset.permission_mode.per_tool_overrides.at("glob") == PermissionMode::AlwaysAllow);
    CHECK(preset.permission_mode.per_tool_overrides.count("list") == 0);
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

TEST_CASE("ParseAgentPresetFrontmatter: a non-bundled tool name decodes as a pattern-map alias "
          "when it is a known registered tool",
          "[agent_preset_frontmatter][patterns]") {
    // "custom_file_tool" is not one of the fixed opencode-shape category words
    // (by_key in agent_preset_frontmatter.cpp), but it IS a live registered
    // tool per known_tool_permission_keys -- exactly the case a renamed or
    // third-party tools-fs replacement would hit (ARCHITECTURE.md, "Core vs.
    // plugin surface": read/write/edit are "not structurally privileged").
    auto preset = ParseWithTools(
        "---\n"
        "name: build\n"
        "permission:\n"
        "  custom_file_tool:\n"
        "    \"**/*.secret\": deny\n"
        "---\n"
        "body\n",
        {"custom_file_tool"});

    REQUIRE(preset.permission_mode.pattern_overrides.count("custom_file_tool") == 1);
    CHECK(preset.permission_mode.pattern_overrides.at("custom_file_tool")[0].glob ==
          "**/*.secret");
    CHECK(preset.permission_mode.pattern_overrides.at("custom_file_tool")[0].mode ==
          PermissionMode::AlwaysDeny);
}

TEST_CASE("ParseAgentPresetFrontmatter: the same key is dropped, not decoded, when the tool "
          "is not known",
          "[agent_preset_frontmatter][patterns]") {
    // Same input as above, but the caller's known_tool_permission_keys says
    // nothing registers under that name -- falls through to the same "no
    // minimal category" warning-and-drop an unknown opencode key gets.
    auto preset = ParseWithTools(
        "---\n"
        "name: build\n"
        "permission:\n"
        "  custom_file_tool:\n"
        "    \"**/*.secret\": deny\n"
        "---\n"
        "body\n",
        {});

    CHECK(preset.permission_mode.pattern_overrides.empty());
}

TEST_CASE("ParseAgentPresetFrontmatter: a scalar bash alias governs bash only",
          "[agent_preset_frontmatter][patterns][compat]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "permission:\n"
        "  bash: allow\n"
        "---\n"
        "body\n");
    // Not the Execute category: python_exec and spawn_subagent keep theirs.
    CHECK_FALSE(preset.permission_mode.execute.has_value());
    CHECK(preset.permission_mode.per_tool_overrides.at("bash") == PermissionMode::AlwaysAllow);
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
    CHECK_THROWS_AS(LoadAgentPresetFrontmatter(path, BundledTools()),
                    std::runtime_error);
}

TEST_CASE("LoadAgentPresetFrontmatter: a normally-sized file is unaffected by the cap",
          "[agent_preset_frontmatter]") {
    testing::TempDir dir;
    const auto path = dir.path() / "normal.md";
    {
        std::ofstream out(path);
        out << "---\nname: build\n---\nbody\n";
    }
    const auto preset = LoadAgentPresetFrontmatter(path, BundledTools());
    CHECK(preset.name == "build");
}

TEST_CASE("ParseAgentPresetFrontmatter: unknown permission_mode keys are reported",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: plan\n"
        "permission_mode:\n"
        "  file_writes: deny\n"
        "  pattern_precedence: specific\n"
        "---\n"
        "body\n");
    CHECK(preset.unknown_permission_keys == std::vector<std::string>{"file_writes"});
    CHECK(preset.permission_mode.pattern_precedence ==
          minimal::tools::PatternPrecedence::Specific);
    // A wrong-SHAPED known block is an error, not a silent no-op.
    CHECK_THROWS(Parse("---\nname: p\npermission_mode:\n  patterns: nope\n---\nb\n"));
}
