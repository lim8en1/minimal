#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "minimal/common/logger.hpp"
#include "skill_access.hpp"
#include "skill_registry.hpp"
#include "skill_tool.hpp"
#include "skills_model.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::skills;
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

    ToolContext MakeContext() const {
        ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = config.dir;
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

TEST_CASE("SkillTool returns the SKILL.md body verbatim, frontmatter stripped, with a header "
          "naming the skill directory",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext();

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
    ToolContext ctx = fixture.MakeContext();
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
    ToolContext ctx = fixture.MakeContext();

    CHECK(fixture.tool.execute({}, ctx).content_text == "missing required string field: name");
    CHECK(fixture.tool.execute({{"name", 42}}, ctx).content_text ==
          "missing required string field: name");
    CHECK(fixture.tool.execute({{"name", ""}}, ctx).content_text ==
          "missing required string field: name");
}

TEST_CASE("SkillTool reports an unknown skill as such", "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext();

    const ToolResult result = fixture.tool.execute({{"name", "nope"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text == "unknown skill: nope");
}

TEST_CASE("SkillTool reports a not-loaded skill with its reason",
          "[skill_tool]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext();

    const ToolResult result = fixture.tool.execute({{"name", "broken"}}, ctx);

    CHECK(result.is_error);
    CHECK(result.content_text ==
          "skill 'broken' is not loaded: missing required frontmatter field: description");
}

TEST_CASE("SkillTool's disclosure offers the stable per-skill grant key",
          "[skill_tool][compat]") {
    SkillFixture fixture;
    ToolContext ctx = fixture.MakeContext();

    const auto disclosure = fixture.tool.Disclose({{"name", "beta"}}, ctx);
    CHECK(disclosure.complete);
    REQUIRE(disclosure.grants.size() == 1);
    CHECK(disclosure.grants.front().key == "skill:beta");
}

// --- the skills permission model and per-preset access -------------------------

namespace {

PermissionAnswer Gate(const PermissionModel& model, const void* layer, const std::string& name,
                      const ToolContext& ctx, const Tool& tool) {
    const nlohmann::json input{{"name", name}};
    return model.Resolve(PermissionStage::kBase,
                         PermissionQuery{tool, "skill", input, ctx, nullptr, layer});
}

}  // namespace

TEST_CASE("the skills model gates the skill tool per name: allow, ask, deny, preload",
          "[skill_tool][skills_model]") {
    SkillFixture fixture;
    const auto model = MakeSkillsPermissionModel();
    CHECK(model->Claims("skill"));
    CHECK_FALSE(model->Claims("read"));

    // The operator allows everything; the preset narrows per name.
    const auto layer = model->BuildLayer(
        {{"*", "allow"}}, {{"beta", "ask"}, {"secret", "deny"}, {"style", "preload"}},
        /*preset_trusted=*/true);
    const ToolContext ctx = fixture.MakeContext();
    CHECK(Gate(*model, layer.get(), "alpha", ctx, fixture.tool).mode == PermissionMode::AlwaysAllow);
    CHECK(Gate(*model, layer.get(), "beta", ctx, fixture.tool).mode == PermissionMode::Ask);
    CHECK(Gate(*model, layer.get(), "secret", ctx, fixture.tool).mode == PermissionMode::AlwaysDeny);
    // Preloaded bodies are already in the prompt: the tool is refused for them.
    CHECK(Gate(*model, layer.get(), "style", ctx, fixture.tool).mode == PermissionMode::AlwaysDeny);

    // No slice anywhere: nothing is allowed.
    const auto empty = model->BuildLayer(nullptr, nullptr, true);
    CHECK(Gate(*model, empty.get(), "alpha", ctx, fixture.tool).mode == PermissionMode::AlwaysDeny);
    CHECK_THROWS(model->BuildLayer(nullptr, {{"alpha", "sometimes"}}, true));
}

TEST_CASE("skill access resolves against the loaded skills and clamps to a parent",
          "[skill_tool][skills_model]") {
    SkillFixture fixture;
    const auto merged = fixture.registry.AllFor(fixture.config.dir);

    // "*" expands to the LOADED skills only; denies resolve out.
    const auto resolved =
        ResolveForRoot({{"*", SkillMode::Allow}, {"beta", SkillMode::Ask}}, merged);
    CHECK(resolved == SkillAccess{{"alpha", SkillMode::Allow}, {"beta", SkillMode::Ask}});

    // The child keeps only what the parent has, at the stricter mode.
    const auto clamped = ClampToParent(resolved, {{"beta", SkillMode::Allow}});
    CHECK(clamped == SkillAccess{{"beta", SkillMode::Ask}});

    CHECK(RenderListing(resolved, merged) ==
          "Available skills:\n- alpha: First skill.\n- beta: Second skill. (asks before loading)");
    CHECK(RenderListing({}, merged).empty());

    const std::string preloads =
        RenderPreloads({{"alpha", SkillMode::Preload}}, fixture.registry, fixture.config.dir);
    CHECK(preloads.find("## Preloaded skills") != std::string::npos);
    CHECK(preloads.find("Alpha body.") != std::string::npos);
}
