#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/agent_preset_frontmatter.hpp"

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
        "provider: openai-main\n"
        "---\n"
        "You are a build agent.\n");

    CHECK(preset.name == "build");
    CHECK(preset.provider == "openai-main");
    CHECK(preset.description.empty());
    CHECK(preset.model.empty());
    CHECK(preset.allowed_tools.empty());
    CHECK(preset.allowed_skills.empty());
    CHECK(preset.allowed_mcp_servers.empty());
    CHECK(preset.compression_prompt.empty());
    CHECK(preset.body == "You are a build agent.\n");
}

TEST_CASE("ParseAgentPresetFrontmatter: missing name throws", "[agent_preset_frontmatter]") {
    CHECK_THROWS_AS(Parse("---\nprovider: openai-main\n---\nbody\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: missing provider throws", "[agent_preset_frontmatter]") {
    CHECK_THROWS_AS(Parse("---\nname: build\n---\nbody\n"), std::runtime_error);
}

TEST_CASE("ParseAgentPresetFrontmatter: allowed_* lists decode", "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "provider: openai-main\n"
        "description: Builds things\n"
        "model: gpt-4.1\n"
        "allowed_tools: [read, write, bash]\n"
        "allowed_skills: [refactor]\n"
        "allowed_mcp_servers: [search]\n"
        "---\n"
        "body\n");

    CHECK(preset.description == "Builds things");
    CHECK(preset.model == "gpt-4.1");
    CHECK(preset.allowed_tools == std::vector<std::string>{"read", "write", "bash"});
    CHECK(preset.allowed_skills == std::vector<std::string>{"refactor"});
    CHECK(preset.allowed_mcp_servers == std::vector<std::string>{"search"});
}

TEST_CASE("ParseAgentPresetFrontmatter: each permission_mode field is independently optional",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "provider: openai-main\n"
        "permission_mode:\n"
        "  file_write: allow\n"
        "---\n"
        "body\n");

    REQUIRE(preset.permission_mode.file_write.has_value());
    CHECK(*preset.permission_mode.file_write == PermissionMode::AlwaysAllow);
    CHECK_FALSE(preset.permission_mode.read_only.has_value());
    CHECK_FALSE(preset.permission_mode.execute.has_value());
    CHECK_FALSE(preset.permission_mode.out_of_workdir.has_value());
}

TEST_CASE("ParseAgentPresetFrontmatter: permission_mode.overrides decodes into per_tool_overrides",
          "[agent_preset_frontmatter]") {
    auto preset = Parse(
        "---\n"
        "name: build\n"
        "provider: openai-main\n"
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
                        "provider: openai-main\n"
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
        "provider: openai-main\n"
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
                        "provider: openai-main\n"
                        "permission_mode:\n"
                        "  execute: maybe\n"
                        "---\n"
                        "body\n"),
                    std::runtime_error);
}
