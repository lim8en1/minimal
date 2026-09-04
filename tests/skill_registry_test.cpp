#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <string>

#include <unistd.h>

#include "minimal/config/skill_registry.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::config;

namespace {

// A throwaway config directory. Every test writes its skills/ tree fresh. The
// directory is unique per process: catch_discover_tests runs every TEST_CASE as
// its own ctest entry, so a fixed path would be wiped out from under a
// concurrent run of this very file (ctest -j).
struct TempConfigDir {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

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

// --- diagnostics that used to mislead ---------------------------------------

TEST_CASE("dotfiles under skills/ are skipped, not reported as broken skills",
          "[skill_registry]") {
    // .DS_Store appears the moment anyone opens the config directory in Finder.
    // Reporting it as a failed skill made the startup listing complain on
    // essentially every macOS machine, and a check that always complains is one
    // nobody reads. A non-dot stray still gets its error (tested above): that is
    // a real mistake worth naming.
    TempConfigDir config;
    config.Write("skills/good/SKILL.md", "---\nname: good\ndescription: Fine.\n---\nBody\n");
    config.Write("skills/.DS_Store", "binary junk");
    config.Write("skills/.hidden/SKILL.md", "---\nname: .hidden\ndescription: d\n---\nB\n");

    const auto registry = SkillRegistry::Load(config.dir);

    REQUIRE(registry.All().size() == 1);
    CHECK(registry.All().front().name == "good");
    CHECK(registry.scan_error().empty());
}

TEST_CASE("an unreadable skills/ is distinguishable from an absent one", "[skill_registry]") {
    // Both yield an empty registry, so without scan_error() a permissions
    // problem is indistinguishable from "skills are simply not configured" --
    // and the operator gets no signal at all.
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\nname: alpha\ndescription: d\n---\nB\n");
    std::filesystem::permissions(config.dir / "skills", std::filesystem::perms::none);

    const auto registry = SkillRegistry::Load(config.dir);
    const std::string error = registry.scan_error();

    std::filesystem::permissions(config.dir / "skills", std::filesystem::perms::owner_all);

    CHECK(registry.All().empty());
    CHECK_FALSE(error.empty());

    // The absent case must stay silent, or the common "no skills configured"
    // state raises a warning on every start. A sibling directory with no
    // skills/ -- not a second TempConfigDir, which shares this one's arena path.
    const auto without_skills = config.dir / "elsewhere";
    std::filesystem::create_directories(without_skills);
    CHECK(SkillRegistry::Load(without_skills).scan_error().empty());
    CHECK(SkillRegistry::Load(without_skills).All().empty());
}

TEST_CASE("an unreadable SKILL.md names the read failure, not frontmatter",
          "[skill_registry]") {
    // Swallowing the read error produced an empty string, which then failed the
    // frontmatter check -- sending the operator, and via SkillTool the model, to
    // fix frontmatter that was never the problem.
    TempConfigDir config;
    config.Write("skills/locked/SKILL.md", "---\nname: locked\ndescription: d\n---\nBody\n");
    std::filesystem::permissions(config.dir / "skills/locked/SKILL.md",
                                 std::filesystem::perms::none);

    const auto registry = SkillRegistry::Load(config.dir);
    const SkillMeta* meta = FindSkill(registry, "locked");
    std::string error = meta == nullptr ? "" : meta->load_error;

    std::filesystem::permissions(config.dir / "skills/locked/SKILL.md",
                                 std::filesystem::perms::owner_all);

    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(error.find("could not be read") != std::string::npos);
    CHECK(error.find("frontmatter") == std::string::npos);
}

TEST_CASE("a non-scalar frontmatter name is an error, not a silent skip", "[skill_registry]") {
    // The name check exists so allowed_skills, the listing, and the file cannot
    // name a skill three different ways. Skipping it for a malformed name
    // disabled the check in exactly the case it should fire.
    TempConfigDir config;
    config.Write("skills/weird/SKILL.md", "---\nname:\n  - a\n  - b\ndescription: d\n---\nBody\n");

    const auto registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "weird");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error.find("must be a string") != std::string::npos);
}

TEST_CASE("a multi-line description is folded to one line", "[skill_registry]") {
    // The description becomes one bullet in the system prompt. Embedded newlines
    // would break that structure inside the cached prefix.
    TempConfigDir config;
    config.Write("skills/multi/SKILL.md",
                 "---\nname: multi\ndescription: |\n  Line one.\n  Line two.\n---\nBody\n");

    const auto registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "multi");
    REQUIRE(meta != nullptr);
    CHECK(meta->loaded);
    CHECK(meta->description == "Line one. Line two.");

}

TEST_CASE("a whitespace-only description is no description at all", "[skill_registry]") {
    // Folding runs before the emptiness check, so "   " must not slip through as
    // a valid description that renders an empty bullet.
    TempConfigDir config;
    config.Write("skills/blank/SKILL.md",
                 "---\nname: blank\ndescription: \"   \"\n---\nBody\n");

    const auto registry = SkillRegistry::Load(config.dir);

    const SkillMeta* meta = FindSkill(registry, "blank");
    REQUIRE(meta != nullptr);  // Find reports unloaded entries too, with a reason
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error.find("description") != std::string::npos);
}

// --- project-local overlay -----------------------------------------------------

TEST_CASE("an empty project root degenerates to the global view", "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\ndescription: a.\n---\nA body.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    CHECK(registry.AllFor({}).size() == registry.All().size());
    CHECK(registry.FindFor({}, "alpha") != nullptr);
    CHECK(registry.FindFor({}, "beta") == nullptr);
}

TEST_CASE("a project skill is discovered and shadows a global one of the same name",
          "[skill_registry]") {
    TempConfigDir config;
    config.Write("skills/alpha/SKILL.md", "---\ndescription: global.\n---\nGlobal body.\n");
    config.Write("skills/only-global/SKILL.md", "---\ndescription: g.\n---\nG.\n");
    // proj carries a .git marker, making it a project root for the walk.
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/skills/alpha/SKILL.md", "---\ndescription: project.\n---\nProject body.\n");
    config.Write("proj/skills/beta/SKILL.md", "---\ndescription: b.\n---\nB body.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);
    // Discovery canonicalizes the root (weakly_canonical), so the scanned
    // candidates carry canonical paths -- the comparison must use them too.
    const auto project_root = std::filesystem::weakly_canonical(config.dir / "proj");

    const auto all = registry.AllFor(project_root);
    REQUIRE(all.size() == 3);  // alpha shadowed in place, beta added

    const SkillMeta* alpha = registry.FindFor(project_root, "alpha");
    REQUIRE(alpha != nullptr);
    CHECK(alpha->loaded);
    CHECK(alpha->body_path == project_root / "skills" / "alpha" / "SKILL.md");
    const auto body = registry.LoadBodyFor(project_root, "alpha");
    REQUIRE(body.has_value());
    CHECK(*body == "Project body.\n");

    // The global view is untouched by the overlay: Find (not FindFor) still
    // sees the global alpha, and the project-only beta is invisible to it.
    CHECK(registry.Find("alpha")->body_path == config.dir / "skills" / "alpha" / "SKILL.md");
    CHECK(registry.Find("beta") == nullptr);
    CHECK(registry.FindFor(project_root, "only-global") != nullptr);
}

TEST_CASE("skill directories are honored along the ancestor walk up to the git root",
          "[skill_registry]") {
    TempConfigDir config;
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/skills/top/SKILL.md", "---\ndescription: t.\n---\nT.\n");
    config.Write("proj/sub/skills/deep/SKILL.md", "---\ndescription: d.\n---\nD.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    // A session rooted at proj/sub sees both: its own, and the worktree root's.
    const auto all = registry.AllFor(config.dir / "proj" / "sub");
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "deep");
    CHECK(all[1].name == "top");
    CHECK(registry.LoadBodyFor(config.dir / "proj" / "sub", "top").has_value());
}

TEST_CASE("without a git root only the project root itself is searched", "[skill_registry]") {
    TempConfigDir config;
    // No .git anywhere in the arena: plain must not reach into its siblings or
    // ancestors, whatever they are.
    config.Write("plain/skills/mine/SKILL.md", "---\ndescription: m.\n---\nM.\n");
    config.Write("outer/skills/theirs/SKILL.md", "---\ndescription: t.\n---\nT.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    const auto all = registry.AllFor(config.dir / "plain");
    REQUIRE(all.size() == 1);
    CHECK(all.front().name == "mine");
    CHECK(registry.FindFor(config.dir / "plain", "theirs") == nullptr);
}

TEST_CASE("the Claude-compatible overlay directories load", "[skill_registry]") {
    TempConfigDir config;
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/.claude/skills/from-claude/SKILL.md",
                 "---\ndescription: c.\n---\nC.\n");
    config.Write("proj/.agents/skills/from-agents/SKILL.md",
                 "---\ndescription: a.\n---\nA.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);
    const auto project_root = config.dir / "proj";

    const auto all = registry.AllFor(project_root);
    REQUIRE(all.size() == 2);
    CHECK(all[0].name == "from-agents");
    CHECK(all[1].name == "from-claude");
    CHECK(registry.LoadBodyFor(project_root, "from-claude").has_value());
}

TEST_CASE("within a project the nearest directory wins on a name collision",
          "[skill_registry]") {
    TempConfigDir config;
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/skills/dupe/SKILL.md", "---\ndescription: root.\n---\nRoot body.\n");
    config.Write("proj/sub/skills/dupe/SKILL.md", "---\ndescription: sub.\n---\nSub body.\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);

    // A session at proj/sub: its OWN dupe is nearer in the walk, so it wins
    // over the worktree root's -- the same nearest-first rule that decides
    // project-over-global.
    const auto body = registry.LoadBodyFor(config.dir / "proj" / "sub", "dupe");
    REQUIRE(body.has_value());
    CHECK(*body == "Sub body.\n");
}

TEST_CASE("a broken project skill surfaces its reason like a global one", "[skill_registry]") {
    TempConfigDir config;
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/skills/broken/README.md", "no skill file here\n");

    const SkillRegistry registry = SkillRegistry::Load(config.dir);
    const auto project_root = config.dir / "proj";

    const SkillMeta* meta = registry.FindFor(project_root, "broken");
    REQUIRE(meta != nullptr);
    CHECK_FALSE(meta->loaded);
    CHECK(meta->load_error == "no SKILL.md in directory");
    CHECK_FALSE(registry.LoadBodyFor(project_root, "broken").has_value());
}
