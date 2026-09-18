// The examples in examples/ are documentation, which is exactly why they rot:
// nobody runs them, so a frontmatter schema change silently turns every file
// into a broken template that teaches the wrong shape. This test parses every
// example with the production parsers -- the same code a config dir goes
// through -- so drift fails here instead of shipping as bad advice.
//
// Scope is deliberately parse-level: agents through LoadAgentPresetFrontmatter
// (the load path, not just the YAML), skills through SkillRegistry::Load (the
// loaded/load_error diagnostics an operator would see), commands through
// ParseCommandFile. Resolution against a live providers.json is NOT tested --
// examples intentionally reference placeholder model refs.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "../../plugins/server/commands-md/commands_md.hpp"
#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/config/frontmatter.hpp"
#include "minimal/config/skill_registry.hpp"
#include "temp_dir.hpp"

using namespace minimal;

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Every .md file in a directory except the README (the READMEs are prose).
std::vector<std::filesystem::path> MdFiles(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md" &&
            entry.path().stem() != "README") {
            files.push_back(entry.path());
        }
    }
    return files;
}

}  // namespace

TEST_CASE("every examples/agents preset parses through the production load path",
          "[examples][agent_preset]") {
    const std::filesystem::path agents_dir = std::filesystem::path(MINIMAL_EXAMPLES_DIR) / "agents";
    REQUIRE(std::filesystem::exists(agents_dir));

    const auto files = MdFiles(agents_dir);
    REQUIRE(files.size() >= 5);

    for (const auto& path : files) {
        SECTION(path.filename().string()) {
            agent::AgentPresetFrontmatter preset;
            REQUIRE_NOTHROW(preset = agent::LoadAgentPresetFrontmatter(
                                path, {"bash", "read", "write", "edit", "glob", "grep"}));
            // The file stem is the address (`--agent research`); a preset whose
            // display name drifts from it would resolve under a name the docs
            // never mention.
            CHECK(preset.name == path.stem().string());
            CHECK(!preset.body.empty());
            CHECK(!preset.description.empty());
            // The zero-tools example is meaningful only while the field stays
            // genuinely absent -- a defaulted empty list would weaken the
            // fail-closed story it demonstrates.
            if (preset.name == "writer") {
                CHECK(preset.allowed_tools.empty());
            }
        }
    }
}

TEST_CASE("every examples/skills skill loads through SkillRegistry", "[examples][skills]") {
    const std::filesystem::path skills_dir =
        std::filesystem::path(MINIMAL_EXAMPLES_DIR) / "skills";
    REQUIRE(std::filesystem::exists(skills_dir));

    // Skills are discovered as <config>/skills/<name>/SKILL.md, so the
    // examples are copied into a temp config dir verbatim and loaded the way
    // the server would -- frontmatter-only scan plus the loaded/load_error
    // diagnostics, not a hand-rolled YAML check.
    testing::TempDir config;
    std::size_t copied = 0;
    for (const auto& entry : std::filesystem::directory_iterator(skills_dir)) {
        if (entry.is_directory() &&
            std::filesystem::exists(entry.path() / "SKILL.md")) {
            std::filesystem::create_directories(config.path() / "skills" / entry.path().filename());
            std::filesystem::copy_file(entry.path() / "SKILL.md",
                                       config.path() / "skills" / entry.path().filename() /
                                           "SKILL.md");
            ++copied;
        }
    }
    REQUIRE(copied >= 3);

    const auto skills = config::SkillRegistry::Load(config.path());
    REQUIRE(skills.All().size() == copied);
    for (const auto& skill : skills.All()) {
        INFO("skill '" << skill.name << "': " << skill.load_error);
        CHECK(skill.loaded);
        CHECK(skill.load_error.empty());
        CHECK(!skill.description.empty());
        // The directory name is the identity; a frontmatter name that
        // disagrees is a load error the copy step would surface.
        CHECK(skills.LoadBody(skill.name).has_value());
    }
}

TEST_CASE("every examples/commands file parses through ParseCommandFile", "[examples][commands]") {
    const std::filesystem::path commands_dir =
        std::filesystem::path(MINIMAL_EXAMPLES_DIR) / "commands";
    REQUIRE(std::filesystem::exists(commands_dir));

    const auto files = MdFiles(commands_dir);
    REQUIRE(files.size() >= 5);

    for (const auto& path : files) {
        SECTION(path.filename().string()) {
            // trusted=true: these are templates for the operator's own config
            // dir, the path where no per-run confirmation applies.
            commands_md::MdCommandSpec spec;
            REQUIRE_NOTHROW(spec = commands_md::ParseCommandFile(path, true));
            CHECK(spec.name == path.stem().string());
            CHECK(!spec.summary.empty());
            // The per-turn override example must actually carry both
            // overrides, or its README row is teaching a no-op.
            if (spec.name == "agent-override") {
                CHECK(!spec.agent.empty());
                CHECK(!spec.model.empty());
            }
        }
    }
}

TEST_CASE("examples/prompts fragments are parseable markdown with non-empty bodies",
          "[examples][prompts]") {
    const std::filesystem::path prompts_dir =
        std::filesystem::path(MINIMAL_EXAMPLES_DIR) / "prompts";
    REQUIRE(std::filesystem::exists(prompts_dir));

    const auto files = MdFiles(prompts_dir);
    REQUIRE(files.size() >= 2);

    for (const auto& path : files) {
        SECTION(path.filename().string()) {
            config::ParsedFrontmatter parsed;
            // A frontmatter block opened but never closed is the one parse
            // error here; the registry strips frontmatter and takes the body.
            REQUIRE_NOTHROW(parsed = config::parse_frontmatter_markdown(ReadFile(path)));
            CHECK(!parsed.body.empty());
        }
    }
}

TEST_CASE("the READMEs' cross-references name files that exist", "[examples][docs]") {
    // The examples READMEs teach by pointing ("pairs with agents/data-analyst.md",
    // "compression_prompt: terse-compression"). A renamed example leaves the
    // advice dangling; these checks pin the pairings the prose promises.
    const std::filesystem::path root = MINIMAL_EXAMPLES_DIR;

    std::set<std::string> agent_names;
    for (const auto& path : MdFiles(root / "agents")) agent_names.insert(path.stem().string());
    CHECK(agent_names.count("data-analyst") == 1);
    CHECK(agent_names.count("ctf") == 1);
    CHECK(agent_names.count("reviewer") == 1);

    CHECK(std::filesystem::exists(root / "prompts" / "terse-compression.md"));
    CHECK(std::filesystem::exists(root / "prompts" / "terse-subagent.md"));
    CHECK(std::filesystem::exists(root / "commands" / "agent-override.md"));

    for (const char* dir : {"repo-etiquette", "eda-checklist", "ctf-triage"}) {
        CHECK(std::filesystem::exists(root / "skills" / dir / "SKILL.md"));
    }
}
