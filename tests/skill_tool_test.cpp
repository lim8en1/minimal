#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "minimal/common/logger.hpp"
#include "minimal/config/skill_registry.hpp"
#include "minimal/tools/skill_tool.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::config;
using namespace minimal::tools;

namespace {

// Unique per process: catch_discover_tests runs every TEST_CASE as its own
// ctest entry, so a fixed path would be wiped out from under a concurrent run
// of this very file (ctest -j).
struct TempConfigDir {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

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

    ToolContext MakeContext(std::vector<ToolContext::SkillAccess> allowed_skills = {}) const {
        ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = config.dir;
        ctx.allowed_skills = std::move(allowed_skills);
        return ctx;
    }

    // Sugar for the common allow-shaped case.
    static ToolContext::SkillAccess Allow(std::string name, bool ask = false) {
        return ToolContext::SkillAccess{std::move(name), ask};
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

TEST_CASE("SkillTool returns the SKILL.md body verbatim, frontmatter stripped, with a header "
          "naming the skill directory",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha")});

    const ToolResult result = fixture.tool.execute({{"name", "alpha"}}, ctx);

    CHECK_FALSE(result.is_error);
    // The header points at the skill's directory (sibling supporting files live
    // there); the body itself is unchanged after the blank-line separator. The
    // registry canonicalizes the project root before scanning it, so the path is
    // compared canonically too (symlinked /var -> /private/var on macOS).
    CHECK(result.content_text ==
          "skill directory: " +
              std::filesystem::weakly_canonical(fixture.config.dir / "skills" / "alpha").string() +
              "\n\nAlpha body.\n");
}

TEST_CASE("SkillTool's header names the global skill directory when the project has no overlay",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha")});
    // A different project root: alpha is no longer a project-local skill and
    // resolves through the global registry, but its directory line must still
    // point at the real files.
    ctx.project_root = fixture.config.dir / "elsewhere";

    const ToolResult result = fixture.tool.execute({{"name", "alpha"}}, ctx);

    CHECK_FALSE(result.is_error);
    CHECK(result.content_text ==
          "skill directory: " +
              std::filesystem::weakly_canonical(fixture.config.dir / "skills" / "alpha").string() +
              "\n\nAlpha body.\n");
}

TEST_CASE("SkillTool rejects a missing, non-string, or empty name", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha")});

    CHECK(fixture.tool.execute({}, ctx).content_text == "missing required string field: name");
    CHECK(fixture.tool.execute({{"name", 42}}, ctx).content_text ==
          "missing required string field: name");
    CHECK(fixture.tool.execute({{"name", ""}}, ctx).content_text ==
          "missing required string field: name");
}

TEST_CASE("SkillTool reports an unknown skill as such", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha")});

    const ToolResult result = fixture.tool.execute({{"name", "nope"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text == "unknown skill: nope");
}

TEST_CASE("SkillTool reports a not-loaded skill with its reason, before the allowed check",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha")});

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
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha")});

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

TEST_CASE("SkillTool admits every skill listed explicitly -- wildcard expansion happened at "
          "resolve time",
          "[skill_tool]") {
    // The tool no longer honours a literal "*": the registry's allowed_skills
    // resolution expands it, so the context arrives with per-skill rows. The
    // broken skill still fails with its reason, not the availability error.
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha"), SkillFixture::Allow("broken")});

    const ToolResult alpha = fixture.tool.execute({{"name", "alpha"}}, ctx);
    CHECK_FALSE(alpha.is_error);
    CHECK(alpha.content_text.ends_with("Alpha body.\n"));

    const ToolResult broken = fixture.tool.execute({{"name", "broken"}}, ctx);
    CHECK(broken.is_error);
    CHECK(broken.content_text.find("is not loaded:") != std::string::npos);
}

// --- ask-mode skills ------------------------------------------------------------

TEST_CASE("SkillTool narrows its own resolution to Ask for an ask-mode skill only",
          "[skill_tool][compat]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("alpha"), SkillFixture::Allow("beta", /*ask=*/true)});

    CHECK(fixture.tool.mode_override_for({{"name", "alpha"}}, ctx) == std::nullopt);
    const auto narrowed = fixture.tool.mode_override_for({{"name", "beta"}}, ctx);
    REQUIRE(narrowed.has_value());
    CHECK(*narrowed == PermissionMode::Ask);
    CHECK(fixture.tool.mode_override_for({{"name", "unlisted"}}, ctx) == std::nullopt);
    CHECK(fixture.tool.mode_override_for({{"name", 42}}, ctx) == std::nullopt);
}

TEST_CASE("SkillTool's disclosure offers the stable per-skill grant key",
          "[skill_tool][compat]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext({SkillFixture::Allow("beta", /*ask=*/true)});

    const auto disclosure = fixture.tool.Disclose({{"name", "beta"}}, ctx);
    CHECK(disclosure.complete);
    REQUIRE(disclosure.grants.size() == 1);
    CHECK(disclosure.grants.front().key == "skill:beta");
}
