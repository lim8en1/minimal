// What the seeded config dir has to be good for.
//
// The seeding itself is CMake's job now (cmake/SeedConfig.cmake, exercised by the
// seed_config CTest cases), so what is left here is the contract on the far side
// of it: that a config dir shaped like the one `cmake --install` produces
// actually resolves into a usable preset.
//
// This exists because of a specific failure. The production compactor reads its
// prompt from <config>/prompts/compression.md; the bundled copy had no delivery
// mechanism at all, and an empty prompt makes compact_messages return "no
// compaction" on every session forever -- visible only as one log line. A feature
// that is silently inert in production passes every test that does not check
// delivery. The same was true of the whole config dir: nothing shipped
// settings.json, permissions.json or agents/*.md either, and the server exited 1
// on a cold start.

#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/agent_preset_registry.hpp"
#include "temp_dir.hpp"

using namespace minimal;

namespace {

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << contents;
}

// The shape SeedConfig.cmake leaves behind, minus whatever the caller overrides.
// providers.json carries a real entry here because these tests resolve a preset;
// the seeded one is deliberately `[]` (see the seed script).
void WriteSeededConfig(const std::filesystem::path& dir) {
    Write(dir / "providers.json",
          R"([{"name":"local","provider":"openai","base_url":"http://x/v1",)"
          R"("model_name":"m","max_tokens":4096}])");
    Write(dir / "settings.json",
          R"({"default_agent_preset":"build","default_provider":"local",)"
          R"("memory":{"auto_compact":true,"context_compaction_trigger":{"percent":80}}})");
    Write(dir / "permissions.json", R"({"default_run_mode":"normal"})");
    Write(dir / "agents" / "build.md",
          "---\nname: build\nprovider: default\n---\nYou are a coding agent.\n");
}

}  // namespace

TEST_CASE("a seeded config dir resolves the default preset", "[bundled_assets]") {
    // The cold-start path: everything the server needs at stage 4, and nothing
    // hand-tuned. If this fails, `cmake --install` followed by a run does not
    // work for a new user.
    testing::TempDir config;
    WriteSeededConfig(config.path());

    const auto skills = config::SkillRegistry::Load(config.path());
    agent::AgentPresetRegistry registry(config.path(), skills);
    const auto& preset = registry.Resolve("build");

    CHECK(preset.name == "build");
    CHECK(preset.provider.model_name == "m");
    REQUIRE(preset.max_tokens.has_value());
    CHECK(*preset.max_tokens == 4096);
}

TEST_CASE("a seeded compression prompt reaches AgentPreset::compression_prompt",
          "[bundled_assets][invariant]") {
    // The whole reason delivery matters: seeding is worth nothing unless
    // AgentPresetRegistry then finds the file. Without prompts/compression.md the
    // preset's compression_prompt is empty and compact_messages short-circuits to
    // "no compaction" on every session, saying so only in a log.
    testing::TempDir config;
    WriteSeededConfig(config.path());

    SECTION("absent, compaction has no prompt and is silently inert") {
        const auto skills = config::SkillRegistry::Load(config.path());
        agent::AgentPresetRegistry registry(config.path(), skills);
        CHECK(registry.Resolve("build").compression_prompt.empty());
    }

    SECTION("present, the default prompt is live") {
        Write(config.path() / "prompts" / "compression.md", "THE DEFAULT COMPACTION PROMPT.\n");
        const auto skills = config::SkillRegistry::Load(config.path());
        agent::AgentPresetRegistry registry(config.path(), skills);
        CHECK(registry.Resolve("build").compression_prompt == "THE DEFAULT COMPACTION PROMPT.\n");
    }
}

TEST_CASE("a preset's compression_prompt override wins over the seeded default",
          "[bundled_assets]") {
    // Why the default is seeded rather than compiled in, stated as a test: an
    // agent can point at a different prompt file, and both live in the config dir
    // where the user can read and edit them.
    testing::TempDir config;
    WriteSeededConfig(config.path());
    Write(config.path() / "prompts" / "compression.md", "THE DEFAULT.\n");
    Write(config.path() / "prompts" / "terse.md", "BE BRIEF.\n");
    Write(config.path() / "agents" / "brief.md",
          "---\nname: brief\nprovider: default\ncompression_prompt: terse\n---\nHi.\n");

    const auto skills = config::SkillRegistry::Load(config.path());
    agent::AgentPresetRegistry registry(config.path(), skills);

    CHECK(registry.Resolve("brief").compression_prompt == "BE BRIEF.\n");
    CHECK(registry.Resolve("build").compression_prompt == "THE DEFAULT.\n");
}
