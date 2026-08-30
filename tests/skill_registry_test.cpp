#include <catch2/catch_test_macros.hpp>

#include <fstream>

#include "minimal/config/skill_registry.hpp"

using namespace minimal;
using namespace minimal::config;

namespace {

// A throwaway config directory. Every test writes its skills/ tree fresh, so a
// fixed arena path (removed up front) is fine and keeps failures inspectable.
struct TempConfigDir {
    std::filesystem::path dir;

    TempConfigDir() {
        dir = std::filesystem::temp_directory_path() / "minimal_skill_registry_test_arena";
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

const SkillMeta* FindSkill(const SkillRegistry& registry, const std::string& name) {
    return registry.Find(name);
}

}  // namespace

TEST_CASE("a valid skill loads with frontmatter resident and body lazy", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/git-release/SKILL.md",
                 "---\n"
                 "name: git-release\n"
                 "description: Cut a release.\n"
                 "---\n"
                 "Bump the version, tag, publish.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    REQUIRE(registry.All().size() == 1);
    const SkillMeta& meta = registry.All().front();
    CHECK(meta.name == "git-release");
    CHECK(meta.description == "Cut a release.");
    CHECK(meta.loaded);
    CHECK(meta.load_error.empty());
    CHECK(meta.body_path == config.dir / "skills" / "git-release" / "SKILL.md");

    const auto body = registry.LoadBody("git-release");
    REQUIRE(body.has_value());
    CHECK(*body == "Bump the version, tag, publish.\n");
}

TEST_CASE("bodies are read at invocation, not at load time", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\ndescription: d.\n---\nOriginal body.\n");

    SkillRegistry registry = SkillRegistry::Load(config.dir);

    // Rewrite the body AFTER loading. A lazy reader sees the new content; an
    // eager one (or a cached one) would not.
    config.Write("skills/alpha/SKILL.md", "---\ndescription: d.\n---\nReplaced body.\n");

    const auto body = registry.LoadBody("alpha");
    REQUIRE(body.has_value());
    CHECK(*body == "Replaced body.\n");
}

TEST_CASE("a directory without SKILL.md is recorded as not loaded", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/broken/README.md", "no skill file here\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    REQUIRE(registry.All().size() == 1);
    const SkillMeta* meta = FindSkill(registry, "broken");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error == "no SKILL.md in directory");
    CHECK_FALSE(registry.LoadBody("broken").has_value());
}

TEST_CASE("a stray file directly under skills/ is recorded as not loaded", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/stray.txt", "---\ndescription: not a directory\n---\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    REQUIRE(registry.All().size() == 1);
    const SkillMeta* meta = FindSkill(registry, "stray.txt");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error == "expected a skill directory containing SKILL.md, found a file");
}

TEST_CASE("an unterminated frontmatter block is recorded with the parse error", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/broken/SKILL.md", "---\nname: broken\ndescription: d.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "broken");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error.rfind("frontmatter parse error:", 0) == 0);
}

TEST_CASE("a SKILL.md with no frontmatter block is recorded as not loaded", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/broken/SKILL.md", "Just a body, no metadata.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "broken");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error == "missing frontmatter block");
}

TEST_CASE("a missing description is recorded as not loaded", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "broken");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error == "missing required frontmatter field: description");
}

TEST_CASE("a frontmatter name that disagrees with the directory is recorded as not loaded",
          "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md",
                 "---\nname: beta\ndescription: d.\n---\nBody.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    // The DIRECTORY name stays the identity, even for a failed entry.
    const SkillMeta* meta = FindSkill(registry, "alpha");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error == "frontmatter name 'beta' does not match 'alpha'");
    CHECK(FindSkill(registry, "beta") == nullptr);
}

TEST_CASE("a frontmatter name matching the directory is accepted", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\nname: alpha\ndescription: d.\n---\nBody.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "alpha");
    REQUIRE(meta != nullptr);
    CHECK(meta->loaded);
}

TEST_CASE("a missing skills directory yields an empty registry", "[skill_registry]") {
    TempConfigDir config;  // no skills/ written at all

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    CHECK(registry.All().empty());
    CHECK(FindSkill(registry, "anything") == nullptr);
}

TEST_CASE("entries are sorted by name regardless of directory iteration order", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/zeta/SKILL.md", "---\ndescription: z.\n---\nZ.\n");
    config.Write("skills/alpha/SKILL.md", "---\ndescription: a.\n---\nA.\n");
    config.Write("skills/mid/SKILL.md", "---\ndescription: m.\n---\nM.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    REQUIRE(registry.All().size() == 3);
    CHECK(registry.All()[0].name == "alpha");
    CHECK(registry.All()[1].name == "mid");
    CHECK(registry.All()[2].name == "zeta");
}

TEST_CASE("one broken skill does not affect its healthy siblings", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\ndescription: a.\n---\nA body.\n");
    config.Write("skills/broken/SKILL.md", "no frontmatter\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    REQUIRE(registry.All().size() == 2);
    const SkillMeta* alpha = FindSkill(registry, "alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->loaded);
    const auto body = registry.LoadBody("alpha");
    REQUIRE(body.has_value());
    CHECK(*body == "A body.\n");

    const SkillMeta* broken = FindSkill(registry, "broken");
    REQUIRE(broken != nullptr);
    CHECK_FALSE(broken->loaded);
}

TEST_CASE("Find returns null for unknown names and LoadBody follows suit", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\ndescription: a.\n---\nA body.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    CHECK(FindSkill(registry, "nope") == nullptr);
    CHECK_FALSE(registry.LoadBody("nope").has_value());
}
