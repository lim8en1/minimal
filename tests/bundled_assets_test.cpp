// Seeding the config dir from the bundled assets that ship with the binary.
//
// This exists because of a specific failure: the production compactor reads its
// prompt from <config>/prompts/compression.md, the bundled copy had no delivery
// mechanism, and an empty prompt makes compact_messages return "no compaction"
// on every session forever -- visible only as one log line. A feature that is
// silently inert in production passes every test that does not check delivery.

#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/common/bundled_assets.hpp"
#include "temp_dir.hpp"

using namespace minimal;

namespace {

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << contents;
}

}  // namespace

TEST_CASE("a missing prompt is seeded into the config dir", "[bundled_assets]") {
    testing::TempDir bundled;
    testing::TempDir config;
    Write(bundled.path() / "compression.md", "SUMMARIZE THIS.\n");

    const auto seeded = common::SeedBundledPrompts(bundled.path(), config.path());

    REQUIRE(seeded.size() == 1);
    CHECK(seeded[0] == "compression.md");
    CHECK(ReadAll(config.path() / "prompts" / "compression.md") == "SUMMARIZE THIS.\n");
}

TEST_CASE("an existing prompt is never overwritten", "[bundled_assets]") {
    // The important half. Reverting someone's edited compaction prompt on every
    // upgrade is worse than shipping without a default at all.
    testing::TempDir bundled;
    testing::TempDir config;
    Write(bundled.path() / "compression.md", "BUNDLED DEFAULT.\n");
    Write(config.path() / "prompts" / "compression.md", "MY OWN PROMPT.\n");

    const auto seeded = common::SeedBundledPrompts(bundled.path(), config.path());

    CHECK(seeded.empty());
    CHECK(ReadAll(config.path() / "prompts" / "compression.md") == "MY OWN PROMPT.\n");
}

TEST_CASE("seeding is per-file, not all-or-nothing", "[bundled_assets]") {
    testing::TempDir bundled;
    testing::TempDir config;
    Write(bundled.path() / "compression.md", "BUNDLED COMPRESSION.\n");
    Write(bundled.path() / "other.md", "BUNDLED OTHER.\n");
    Write(config.path() / "prompts" / "compression.md", "MINE.\n");

    const auto seeded = common::SeedBundledPrompts(bundled.path(), config.path());

    REQUIRE(seeded.size() == 1);
    CHECK(seeded[0] == "other.md");
    CHECK(ReadAll(config.path() / "prompts" / "compression.md") == "MINE.\n");
    CHECK(ReadAll(config.path() / "prompts" / "other.md") == "BUNDLED OTHER.\n");
}

TEST_CASE("a missing bundle directory is a no-op, not a throw", "[bundled_assets]") {
    // An install that shipped no prompts must still start. The compactor already
    // degrades to no-compaction; a hard failure here would be worse than the gap.
    testing::TempDir config;
    CHECK(common::SeedBundledPrompts(config.path() / "does-not-exist", config.path()).empty());
    CHECK(common::SeedBundledPrompts({}, config.path()).empty());
}

TEST_CASE("a seeded prompt reaches AgentPreset::compression_prompt", "[bundled_assets]") {
    // The whole chain, and the reason the other cases matter: seeding is only
    // worth anything if AgentPresetRegistry then finds the file. Without the seed
    // the preset's compression_prompt is empty and compact_messages short-circuits
    // to "no compaction" on every session.
    testing::TempDir bundled;
    testing::TempDir config;
    Write(bundled.path() / "compression.md", "THE DEFAULT COMPACTION PROMPT.\n");
    Write(config.path() / "providers.json",
          R"([{"name":"main","provider":"fake","base_url":"http://x/v1","model_name":"m"}])");
    Write(config.path() / "settings.json", R"({"default_agent_preset":"build"})");
    Write(config.path() / "permissions.json", "{}");
    Write(config.path() / "agents" / "build.md", "---\nname: build\nprovider: main\n---\nHi.\n");

    SECTION("without seeding, compaction has no prompt and is silently inert") {
        const auto skills = config::SkillRegistry::Load(config.path());
        agent::AgentPresetRegistry registry(config.path(), skills);
        CHECK(registry.Resolve("build").compression_prompt.empty());
    }

    SECTION("after seeding, the default prompt is live") {
        REQUIRE(common::SeedBundledPrompts(bundled.path(), config.path()).size() == 1);
        const auto skills = config::SkillRegistry::Load(config.path());
        agent::AgentPresetRegistry registry(config.path(), skills);
        CHECK(registry.Resolve("build").compression_prompt == "THE DEFAULT COMPACTION PROMPT.\n");
    }
}
