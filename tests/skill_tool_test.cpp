#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>
#include <vector>

#include "minimal/common/logger.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/tools/skill_tool.hpp"

using namespace minimal;
using namespace minimal::config;
using namespace minimal::tools;

namespace {

struct TempConfigDir {
    std::filesystem::path dir;

    TempConfigDir() {
        dir = std::filesystem::temp_directory_path() / "minimal_skill_tool_test_arena";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
    }
    ~TempConfigDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }
};

// Two healthy skills and one broken one -- enough surface to exercise every
// branch of the error ladder. Written BEFORE Load: the registry is a snapshot
// taken at construction, so order matters here.
void WriteDefaultSkills(const std::filesystem::path& config_dir) {
    auto write = [&config_dir](const std::string& relative, const std::string& contents) {
        const auto path = config_dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    };
    write("skills/alpha/SKILL.md",
          "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");
    write("skills/beta/SKILL.md",
          "---\nname: beta\ndescription: Second skill.\n---\nBeta body.\n");
    // Missing description -> not loaded, with a reason.
    write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");
}

SkillRegistry LoadDefaultSkills(const std::filesystem::path& config_dir) {
    WriteDefaultSkills(config_dir);
    return SkillRegistry::Load(config_dir);
}

struct SkillFixture {
    TempConfigDir config;
    SkillRegistry registry;
    SkillTool tool;

    // Member initialisation order is declaration order: config, then the
    // snapshot taken from it, then the tool bound to that snapshot.
    SkillFixture() : registry(LoadDefaultSkills(config.dir)), tool(registry) {}

    ToolContext MakeContext(std::vector<std::string> allowed_skills = {}) const {
        ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = config.dir;
        ctx.allowed_skills = std::move(allowed_skills);
        return ctx;
    }
};

}  // namespace

TEST_CASE("SkillTool spec: name, ReadOnly category, and the input schema", "[skill_tool]") {
    SkillFixture fixture;

    const ToolSpec& spec = fixture.tool.spec();
    CHECK(spec.name == "skill");
    CHECK(spec.category == ToolCategory::ReadOnly);
    CHECK(spec.definition.name == "skill");
    CHECK_FALSE(spec.definition.description.empty());
    CHECK(spec.definition.input_schema.at("type") == "object");
    CHECK(spec.definition.input_schema.at("required") == nlohmann::json::array({"name"}));
    CHECK(spec.definition.input_schema.at("properties").at("name").at("type") == "string");
}

TEST_CASE("SkillTool returns the SKILL.md body verbatim, frontmatter stripped", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({"alpha"});

    const ToolResult result = fixture.tool.execute({{"name", "alpha"}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.content_text == "Alpha body.\n");
}

TEST_CASE("SkillTool rejects a missing, non-string, or empty name", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({"alpha"});

    CHECK(fixture.tool.execute({}, ctx).content_text == "missing required string field: name");
    CHECK(fixture.tool.execute({{"name", 42}}, ctx).content_text ==
          "missing required string field: name");
    CHECK(fixture.tool.execute({{"name", ""}}, ctx).content_text ==
          "missing required string field: name");
}

TEST_CASE("SkillTool reports an unknown skill as such", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({"alpha"});

    const ToolResult result = fixture.tool.execute({{"name", "nope"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text == "unknown skill: nope");
}

TEST_CASE("SkillTool reports a not-loaded skill with its reason, before the allowed check",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({"alpha"});

    // `broken` is ALSO not in allowed_skills -- the load failure must win, since
    // it is unusable by every preset and names the config problem to fix.
    const ToolResult result = fixture.tool.execute({{"name", "broken"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text ==
          "skill 'broken' is not loaded: missing required frontmatter field: description");
}

TEST_CASE("SkillTool rejects a loaded skill the preset does not allow, listing what is",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({"alpha"});

    const ToolResult result = fixture.tool.execute({{"name", "beta"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text ==
          "skill 'beta' is not available in this session. Available skills: alpha");
}

TEST_CASE("SkillTool with no allowed skills reports none available", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext();

    const ToolResult result = fixture.tool.execute({{"name", "alpha"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text ==
          "skill 'alpha' is not available in this session. Available skills: (none)");
}

TEST_CASE("SkillTool honours the wildcard allowed_skills entry", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({"*"});

    const ToolResult alpha = fixture.tool.execute({{"name", "alpha"}}, ctx);
    CHECK_FALSE(alpha.is_error);
    CHECK(alpha.content_text == "Alpha body.\n");

    // The wildcard admits every LOADED skill -- the broken one still fails with
    // its reason.
    const ToolResult broken = fixture.tool.execute({{"name", "broken"}}, ctx);
    CHECK(broken.is_error);
    CHECK(broken.content_text.find("is not loaded:") != std::string::npos);
}
