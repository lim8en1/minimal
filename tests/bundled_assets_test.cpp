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

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/permission_resolution.hpp"
#include "minimal/tools/tool_executor.hpp"
#include "minimal/tools/tool_registry.hpp"

#include "background_registry.hpp"
#include "bash_session_state.hpp"
#include "bash_settings.hpp"
#include "bash_tool.hpp"
#include "fakes.hpp"
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
          R"({"providers":{"local":{"backend":"openai","url":"http://x/v1","max_tokens":4096,)"
          R"("models":{"m":{}}}}})");
    Write(dir / "settings.json",
          R"({"default_agent_preset":"build","default_model":"local/m",)"
          R"("memory":{"auto_compact":true,"context_compaction_trigger":{"percent":80}}})");
    Write(dir / "permissions.json", R"({"default_run_mode":"normal"})");
    Write(dir / "agents" / "build.md",
          "---\nname: build\n---\nYou are a coding agent.\n");  // takes default_model
}

}  // namespace

TEST_CASE("a seeded config dir resolves the default preset", "[bundled_assets]") {
    // The cold-start path: everything the server needs at stage 4, and nothing
    // hand-tuned. If this fails, `cmake --install` followed by a run does not
    // work for a new user.
    testing::TempDir config;
    WriteSeededConfig(config.path());

    agent::AgentPresetRegistry registry(config.path(),
                                         config::LoadSettingsConfig(config.path() / "settings.json"));
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
        agent::AgentPresetRegistry registry(config.path(),
                                         config::LoadSettingsConfig(config.path() / "settings.json"));
        CHECK(registry.Resolve("build").compression_prompt.empty());
    }

    SECTION("present, the default prompt is live") {
        Write(config.path() / "prompts" / "compression.md", "THE DEFAULT COMPACTION PROMPT.\n");
        agent::AgentPresetRegistry registry(config.path(),
                                         config::LoadSettingsConfig(config.path() / "settings.json"));
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
          "---\nname: brief\ncompression_prompt: terse\n---\nHi.\n");

    agent::AgentPresetRegistry registry(config.path(),
                                         config::LoadSettingsConfig(config.path() / "settings.json"));

    CHECK(registry.Resolve("brief").compression_prompt == "BE BRIEF.\n");
    CHECK(registry.Resolve("build").compression_prompt == "THE DEFAULT.\n");
}

TEST_CASE("every bundled/agents preset parses through the production load path",
          "[bundled_assets][agent_preset]") {
    // The shipped defaults are config-dir content like any other, and the same
    // rot that took examples/ down (see examples_parse_test.cpp) would take these:
    // nobody loads them in a test, so a frontmatter schema change turns each file
    // into a broken default that fails only on a user's cold start.
    //
    // The stem is the address (FindPresetPath resolves agents/<stem>.md; --agent,
    // /agent and spawn all name it), so a frontmatter `name:` that drifts from the
    // stem resolves fine but surfaces the wrong identity -- the tab label and loop
    // attribution read preset.name, and a bundled review.md carrying `name: plan`
    // shipped exactly that way. Asserted per file below, not once.
    const std::filesystem::path agents_dir =
        std::filesystem::path(MINIMAL_BUNDLED_DIR) / "agents";
    REQUIRE(std::filesystem::exists(agents_dir));

    std::size_t checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(agents_dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
        ++checked;
        SECTION(entry.path().filename().string()) {
            agent::AgentPresetFrontmatter preset;
            REQUIRE_NOTHROW(preset = agent::LoadAgentPresetFrontmatter(
                                entry.path(), std::vector<std::string>{"bash", "read", "write", "edit", "glob", "grep"}));
            CHECK(preset.name == entry.path().stem().string());
            CHECK(!preset.body.empty());
            CHECK(!preset.description.empty());
        }
    }
    // The set is five today (build, explore, general, plan, review); the bound
    // catches the agents/ directory going missing under a renamed checkout root
    // without pinning the test to every future preset addition.
    CHECK(checked >= 2);
}

TEST_CASE("the bundled plan preset: bash is deny + atomic allow, write is denied, in every run mode",
          "[bundled][plan][permissions]") {
    // Loads the real bundled/agents/plan.md and drives it through a real
    // ToolExecutor + bash tool. Bypass must change nothing here: it approves
    // prompts, and this preset asks nothing -- every outcome is allow or deny.
    const auto preset = agent::LoadAgentPresetFrontmatter(
        std::filesystem::path(MINIMAL_BUNDLED_DIR) / "agents" / "plan.md",
        std::vector<std::string>{"bash", "read", "write", "edit", "glob", "grep"});
    const auto policy = agent::BuildPermissionPolicy(preset.permission_mode, {},
                                                     /*preset_is_trusted=*/true,
                                                     {"read", "write", "edit"});

    for (const auto run_mode : {tools::PermissionRunMode::Normal, tools::PermissionRunMode::Bypass,
                                tools::PermissionRunMode::AllowOnly}) {
        testing::TempDir dir;
        tools_bash::BashSessionStateStore store;
        tools_bash::BackgroundTaskRegistry background;
        tools::ToolRegistry registry;
        registry.Register(tools_bash::MakeBashTool(tools_bash::BashSettings{}, store, background));
        auto write = std::make_unique<testing::FakeTool>("write", tools::ToolCategory::FileWrite);
        write->set_path_key("path");
        auto* write_raw = write.get();
        registry.Register(std::move(write));
        auto edit = std::make_unique<testing::FakeTool>("edit", tools::ToolCategory::FileWrite);
        edit->set_path_key("path");
        auto* edit_raw = edit.get();
        registry.Register(std::move(edit));

        testing::UnreachablePermissionChecker checker;
        tools::ToolExecutor executor(registry, checker, policy, {}, run_mode);
        tools::ToolContext ctx{common::DefaultLogger()};
        ctx.session_id = "s1";
        ctx.project_root = dir.path();
        ctx.emit_output = [](std::string_view) {};
        int n = 0;
        auto bash = [&](const std::string& command) {
            return executor.Run("t" + std::to_string(++n), "bash", {{"command", command}}, ctx);
        };

        SECTION("atomic read-only programs run, alone and piped") {
            CHECK_FALSE(bash("pwd").is_error);
            CHECK_FALSE(bash("ls").is_error);
            CHECK_FALSE(bash("ls | wc -l").is_error);
        }
        SECTION("everything not on the list is denied outright, never prompted") {
            CHECK(bash("touch made.txt").is_error);
            CHECK(bash("rm -rf .").is_error);
            CHECK(bash("sed -i s/a/b/ f").is_error);
            CHECK(bash("git commit -m x").is_error);
            CHECK_FALSE(std::filesystem::exists(dir.path() / "made.txt"));
        }
        SECTION("one uncovered stage denies the whole pipeline or list") {
            CHECK(bash("ls | tee out.txt").is_error);
            CHECK(bash("ls; touch made.txt").is_error);
            CHECK(bash("ls && touch made.txt").is_error);
            CHECK(bash("ls $(touch made.txt)").is_error);
            CHECK_FALSE(std::filesystem::exists(dir.path() / "made.txt"));
        }
        SECTION("an output redirect makes an allowed program ineligible") {
            CHECK(bash("ls > out.txt").is_error);
            CHECK(bash("pwd >> out.txt").is_error);
            CHECK(bash("git log > out.txt").is_error);
            CHECK_FALSE(std::filesystem::exists(dir.path() / "out.txt"));
        }
        SECTION("an unparseable program name is not an allowed program") {
            CHECK(bash("P=touch; $P made.txt").is_error);
            CHECK(bash("eval \"touch made.txt\"").is_error);
            CHECK_FALSE(std::filesystem::exists(dir.path() / "made.txt"));
        }
        SECTION("flags that turn an allowed program into a writer or launcher are denied") {
            CHECK(bash("git log --output=out.txt").is_error);
            CHECK(bash("git diff --output=out.txt").is_error);
            CHECK(bash("rg --pre=touch x").is_error);
            CHECK_FALSE(std::filesystem::exists(dir.path() / "out.txt"));
        }
        SECTION("write/edit: only .minimal/plans/** is open, .env never") {
            for (const auto* tool : {"write", "edit"}) {
                CHECK_FALSE(executor
                                .Run("p", tool, {{"path", ".minimal/plans/topic.md"}}, ctx)
                                .is_error);
                CHECK(executor.Run("s", tool, {{"path", "src/a.cpp"}}, ctx).is_error);
                CHECK(executor.Run("e", tool, {{"path", ".env"}}, ctx).is_error);
                CHECK(executor.Run("n", tool, {{"path", ".minimal/plans/.env"}}, ctx).is_error);
            }
            CHECK(write_raw->calls.size() == 1);
            CHECK(edit_raw->calls.size() == 1);
        }
        CHECK_FALSE(checker.called);  // nothing was ever ask-gated
    }
}
