#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <fstream>
#include <string>

#include <unistd.h>

#include "minimal/agent/agent_preset_registry.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using namespace minimal::agent;
using minimal::tools::PermissionMode;
using minimal::tools::ToolCategory;

namespace {

// Unique per process: catch_discover_tests runs every TEST_CASE as its own
// ctest entry, so a fixed path would be wiped out from under a concurrent run
// of this very file (ctest -j).
struct TempConfigDir {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();

    TempConfigDir() {
        std::filesystem::create_directories(dir / "agents");
        std::filesystem::create_directories(dir / "prompts");
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void WriteJson(const std::string& relative, const nlohmann::json& doc) const { Write(relative, doc.dump()); }

    // One provider, one model, addressed as "openai-main/gpt-4o-mini".
    static nlohmann::json DefaultProviders() {
        return {{"providers",
                 {{"openai-main",
                   {{"backend", "openai"},
                    {"url", "https://api.openai.com/v1"},
                    {"parameters", {{"temperature", 0.5}}},
                    // Two, so a preset choosing between them is expressible --
                    // the thing the flat schema could not do.
                    {"models",
                     {{"gpt-4o-mini", nlohmann::json::object()},
                      {"gpt-4.1", {{"max_tokens", 8192}}},
                      {"small", {{"context_window_tokens", 32768}}}}}}}}}};
    }

    // A minimal, valid tree every test can start from and then perturb.
    void WriteDefaults() const {
        WriteJson("providers.json", DefaultProviders());
        WriteJson("settings.json", nlohmann::json::object());
        WriteJson("permissions.json", nlohmann::json::object());
        Write("agents/build.md",
              "---\n"
              "name: build\n"
              "model: openai-main/gpt-4o-mini\n"
              "---\n"
              "You are a build agent.\n");
    }
};

}  // namespace

TEST_CASE("AgentPresetRegistry: happy path resolves provider fields and system prompt", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const AgentPreset& preset = registry.Resolve("build");
    CHECK(preset.name == "build");
    CHECK(preset.provider.base_url == "https://api.openai.com/v1");
    CHECK(preset.provider.model_name == "gpt-4o-mini");
    CHECK(preset.provider.api_key.empty());
    CHECK(preset.parameters == nlohmann::json{{"temperature", 0.5}});
    CHECK_FALSE(preset.context_window_tokens.has_value());
    CHECK(preset.system_prompt == "You are a build agent.\n");
    CHECK_FALSE(preset.is_subagent);
}

TEST_CASE("AgentPresetRegistry: the model's context_window_tokens reaches the resolved preset",
          "[agent_preset_registry]") {
    // Load-bearing for the window-relative compaction triggers: they resolve
    // against the preset's window, so a value dropped here would silently
    // disable percent/buffer triggers on every session. PER MODEL, not per
    // provider entry -- two models on one endpoint have different windows, which
    // is the case the flat schema could not express at all.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\nname: build\nmodel: openai-main/small\n---\nbody\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const AgentPreset& preset = registry.Resolve("build");
    REQUIRE(preset.context_window_tokens.has_value());
    CHECK(*preset.context_window_tokens == 32768);
}

TEST_CASE("AgentPresetRegistry: a preset picks which of a provider's models it uses",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4.1\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto& preset = registry.Resolve("build");
    CHECK(preset.provider.model_name == "gpt-4.1");
    // Same connection, that model's own cap.
    CHECK(preset.provider.base_url == "https://api.openai.com/v1");
    REQUIRE(preset.max_tokens.has_value());
    CHECK(*preset.max_tokens == 8192);
}

TEST_CASE("AgentPresetRegistry: a preset with no model takes settings.default_model",
          "[agent_preset_registry]") {
    // `provider: default` used to spell this. A preset now simply omits `model`,
    // which is one fewer magic literal and reads as what it means.
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("settings.json", {{"default_model", "openai-main/gpt-4o-mini"}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").provider.base_url == "https://api.openai.com/v1");
}

TEST_CASE("AgentPresetRegistry: default_model comes from the settings it was GIVEN",
          "[agent_preset_registry][invariant]") {
    // settings.json used to be parsed a third time inside the registry, so main
    // could run the loop on one parse while the registry resolved defaults from
    // another. It must use the caller's parse: here the file on disk names
    // gpt-4o-mini and the passed settings name gpt-4.1, and the resolved
    // max_tokens (8192, declared only on gpt-4.1) is the discriminator.
    // Fails with the revert: the registry would re-read the file and resolve
    // gpt-4o-mini, which declares no max_tokens at all.
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("settings.json", {{"default_model", "openai-main/gpt-4o-mini"}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);

    auto passed = config::LoadSettingsConfig(config.dir / "settings.json");
    passed.default_model = "openai-main/gpt-4.1";
    AgentPresetRegistry registry(config.dir, skills, passed);

    const auto preset = registry.Resolve("build");
    CHECK(preset.model_ref.str() == "openai-main/gpt-4.1");
    // gpt-4.1 alone declares max_tokens 8192; gpt-4o-mini declares nothing.
    REQUIRE(preset.max_tokens.has_value());
    CHECK(*preset.max_tokens == 8192);
}

TEST_CASE("AgentPresetRegistry: no model and no default_model throws",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("settings.json", nlohmann::json::object());
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK_THROWS_AS(registry.Resolve("build"), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: an unknown model names what is available",
          "[agent_preset_registry]") {
    // A miss here is nearly always a typo or an unconfigured provider, and both
    // are answered by seeing the list rather than by being told the name was
    // wrong.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: nonexistent/model\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    try {
        registry.Resolve("build");
        FAIL("expected an unknown model to throw");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("nonexistent/model") != std::string::npos);
        CHECK(message.find("openai-main/gpt-4o-mini") != std::string::npos);
    }
}

TEST_CASE("AgentPresetRegistry: a preset naming a discover-only model resolves through discovery",
          "[agent_preset_registry][discover]") {
    // Track N: providers.json need not name every model. The preset resolves at
    // first use, the catalog is consulted lazily through the injected hook, and
    // the discovered context window reaches the preset -- which is what the
    // window-relative compaction triggers read.
    TempConfigDir config;
    config.WriteJson("providers.json",
                     {{"providers",
                       {{"disc",
                         {{"backend", "openai"},
                          {"url", "http://localhost:8000/v1"},
                          {"discover", true}}}}}});
    config.WriteJson("settings.json", nlohmann::json::object());
    config.WriteJson("permissions.json", nlohmann::json::object());
    config.Write("agents/build.md",
                 "---\nname: build\nmodel: disc/served-model\n---\nbody\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                 config::LoadSettingsConfig(config.dir / "settings.json"));

    int calls = 0;
    registry.SetDiscoveryHook(
        [&calls](const std::string& kind, const llm::ProviderConfig& c) {
            ++calls;
            CHECK(kind == "openai");
            CHECK(c.base_url == "http://localhost:8000/v1");
            return config::DiscoveryOutcome{
                /*catalog_present=*/true, /*ok=*/true, {},
                {llm::DiscoveredModel{"served-model", 262144, nlohmann::json::object()}}};
        });

    const AgentPreset& preset = registry.Resolve("build");
    REQUIRE(preset.context_window_tokens.has_value());
    CHECK(*preset.context_window_tokens == 262144);
    CHECK(preset.provider.model_name == "served-model");
    CHECK(calls == 1);  // the fetch happened once, at first resolve

    // And a preset naming a model the catalog does not serve reports why.
    config.Write("agents/other.md",
                 "---\nname: other\nmodel: disc/absent\n---\nbody\n");
    bool threw = false;
    try {
        registry.Resolve("other");
    } catch (const std::runtime_error& error) {
        threw = true;
        const std::string message = error.what();
        CHECK(message.find("disc/absent") != std::string::npos);
        // The listing shows the DISCOVERED model, not nothing -- the miss is
        // distinguishable from a dead catalog, which names the failure instead.
        CHECK(message.find("disc/served-model") != std::string::npos);
    }
    CHECK(threw);
    CHECK(calls == 1);  // negative answer, but no refetch: negative-cached
}

TEST_CASE("AgentPresetRegistry: unknown preset name throws", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK_THROWS_AS(registry.Resolve("nonexistent"), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: permission chain layers take effect in priority order",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json", {{"fallback", {{"execute", "deny"}}},
                                           {"per_tool_overrides", {{"bash", "allow"}}}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "permission_mode:\n"
                  "  execute: ask\n"
                  "  overrides:\n"
                  "    bash: deny\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto& policy = registry.Resolve("build").permission_mode;
    // preset per-tool override beats everything, including global per-tool.
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysDeny);
    // preset category beats global fallback for a tool with no per-tool rule.
    CHECK(policy.mode_for("other", ToolCategory::Execute) == PermissionMode::Ask);
}

TEST_CASE("AgentPresetRegistry: global per-tool override applies when preset is silent",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json", {{"per_tool_overrides", {{"bash", "allow"}}}});
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").permission_mode.mode_for("bash", ToolCategory::Execute) ==
          PermissionMode::AlwaysAllow);
}

TEST_CASE("AgentPresetRegistry: falls to hard-coded default with no config at all", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").permission_mode.mode_for("anything", ToolCategory::Execute) ==
          PermissionMode::Ask);
}

TEST_CASE("AgentPresetRegistry: mcp_permissions per-server vs. global default", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json", {{"mcp_default_mode", "deny"}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "mcp_permissions:\n"
                  "  search: allow\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto& mcp = registry.Resolve("build").mcp_permissions;
    CHECK(mcp.mode_for("search") == PermissionMode::AlwaysAllow);
    CHECK(mcp.mode_for("unconfigured") == PermissionMode::AlwaysDeny);
}

TEST_CASE("AgentPresetRegistry: compression_prompt loads the named prompts/<x>.md", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/custom.md", "Custom compaction prompt.\n");
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "compression_prompt: custom\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").compression_prompt == "Custom compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: compression_prompt falls back to prompts/compression.md when omitted",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/compression.md", "Default compaction prompt.\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").compression_prompt == "Default compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: missing default compression prompt yields empty string, no throw",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").compression_prompt.empty());
}

TEST_CASE("AgentPresetRegistry: an explicitly-named missing compression prompt throws",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "compression_prompt: nonexistent\n"
                  "---\n"
                  "body\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK_THROWS_AS(registry.Resolve("build"), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: missing prompts/subagent.md yields an empty SubagentPrefix", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.SubagentPrefix().empty());
}

TEST_CASE("AgentPresetRegistry: present prompts/subagent.md is read verbatim", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/subagent.md", "You are in subagent mode.\n");
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.SubagentPrefix() == "You are in subagent mode.\n");
}

TEST_CASE("AgentPresetRegistry: malformed providers.json throws at construction", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("providers.json", "{not valid json");

    config::SkillRegistry skills;  // not referenced: construction throws before the listing renders
    CHECK_THROWS_AS(AgentPresetRegistry(config.dir, skills,
                                     config::LoadSettingsConfig(config.dir / "settings.json")),
                     std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: malformed permissions.json throws at construction", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("permissions.json", "{not valid json");

    config::SkillRegistry skills;  // not referenced: construction throws before the listing renders
    CHECK_THROWS_AS(AgentPresetRegistry(config.dir, skills,
                                     config::LoadSettingsConfig(config.dir / "settings.json")),
                     std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: Resolve() caches -- a second call survives the file being deleted",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config::SkillRegistry skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const std::string first_name = registry.Resolve("build").name;
    std::filesystem::remove(config.dir / "agents" / "build.md");

    CHECK(registry.Resolve("build").name == first_name);
}

TEST_CASE("AgentPresetRegistry: allowed skills render into the system prompt, sorted by name",
          "[agent_preset_registry][skills]") {
    TempConfigDir config;
    config.WriteDefaults();
    // allowed_skills lists beta FIRST on purpose: the listing must come out
    // sorted (it sits in the cached prefix and cannot reorder turn to turn).
    config.Write("agents/build.md",
                 "---\n"
                 "name: build\n"
                 "model: openai-main/gpt-4o-mini\n"
                 "allowed_skills:\n"
                 "  - beta\n"
                 "  - alpha\n"
                 "---\n"
                 "You are a build agent.\n");
    config.Write("skills/alpha/SKILL.md",
                 "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");
    config.Write("skills/beta/SKILL.md",
                 "---\nname: beta\ndescription: Second skill.\n---\nBeta body.\n");
    config.Write("skills/gamma/SKILL.md",
                 "---\nname: gamma\ndescription: Not allowed here.\n---\nGamma body.\n");

    auto skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").system_prompt == "You are a build agent.\n"
                                                     "\n## Available skills\n\n"
                                                     "Load a skill's full instructions with the `skill` tool.\n"
                                                     "- alpha: First skill.\n"
                                                     "- beta: Second skill.\n");
}

TEST_CASE("AgentPresetRegistry: allowed_skills resolves to exactly what the listing advertises",
          "[agent_preset_registry][skills]") {
    // The listing and AgentLoop's tool-exposure gate must be driven by the same
    // set. They used to be computed separately -- the listing from
    // allowed_skills ∩ loaded, the gate from allowed_skills being non-empty --
    // so `allowed_skills: [typo]` produced a preset that advertised nothing and
    // still offered the model a `skill` tool no call could satisfy.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\n"
                 "name: build\n"
                 "model: openai-main/gpt-4o-mini\n"
                 "allowed_skills:\n"
                 "  - alpha\n"
                 "  - typo\n"      // never existed
                 "  - broken\n"    // exists but failed to load
                 "---\n"
                 "You are a build agent.\n");
    config.Write("skills/alpha/SKILL.md",
                 "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");
    config.Write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");

    auto skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    const auto preset = registry.Resolve("build");

    CHECK(preset.allowed_skills == std::vector<std::string>{"alpha"});
    CHECK(preset.system_prompt.find("- alpha: First skill.") != std::string::npos);
    CHECK(preset.system_prompt.find("typo") == std::string::npos);
    CHECK(preset.system_prompt.find("broken") == std::string::npos);
}

TEST_CASE("AgentPresetRegistry: allowed_skills naming only unusable skills resolves empty",
          "[agent_preset_registry][skills]") {
    // Empty is what withholds the `skill` tool downstream, so this is the case
    // that must not come back non-empty.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\n"
                 "name: build\n"
                 "model: openai-main/gpt-4o-mini\n"
                 "allowed_skills:\n"
                 "  - typo\n"
                 "---\n"
                 "You are a build agent.\n");
    config.Write("skills/alpha/SKILL.md",
                 "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");

    auto skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    const auto preset = registry.Resolve("build");

    CHECK(preset.allowed_skills.empty());
    CHECK(preset.system_prompt == "You are a build agent.\n");
}

TEST_CASE("AgentPresetRegistry: allowed_skills ['*'] resolves to the loaded skills by name",
          "[agent_preset_registry][skills]") {
    // Expanded at resolution time, not carried through as a literal "*", so
    // every consumer downstream sees a plain sorted list.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4o-mini\n"
                 "allowed_skills:\n  - \"*\"\n---\nYou are a build agent.\n");
    config.Write("skills/beta/SKILL.md",
                 "---\nname: beta\ndescription: Second skill.\n---\nBeta body.\n");
    config.Write("skills/alpha/SKILL.md",
                 "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");
    config.Write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");

    auto skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").allowed_skills ==
          (std::vector<std::string>{"alpha", "beta"}));
}

TEST_CASE("AgentPresetRegistry: a skill that failed to load is excluded from the listing",
          "[agent_preset_registry][skills]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\n"
                 "name: build\n"
                 "model: openai-main/gpt-4o-mini\n"
                 "allowed_skills:\n"
                 "  - broken\n"
                 "---\n"
                 "You are a build agent.\n");
    // Missing description -> not loaded. It must not appear in the listing; its
    // reason surfaces through SkillTool at invocation instead.
    config.Write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");

    auto skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").system_prompt == "You are a build agent.\n");
}

TEST_CASE("AgentPresetRegistry: allowed_skills ['*'] lists every loaded skill",
          "[agent_preset_registry][skills]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\n"
                 "name: build\n"
                 "model: openai-main/gpt-4o-mini\n"
                 "allowed_skills:\n"
                 "  - \"*\"\n"
                 "---\n"
                 "You are a build agent.\n");
    config.Write("skills/alpha/SKILL.md",
                 "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");
    config.Write("skills/beta/SKILL.md",
                 "---\nname: beta\ndescription: Second skill.\n---\nBeta body.\n");

    auto skills = config::SkillRegistry::Load(config.dir);
    AgentPresetRegistry registry(config.dir, skills,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const std::string prompt = registry.Resolve("build").system_prompt;
    CHECK(prompt.find("- alpha: First skill.") != std::string::npos);
    CHECK(prompt.find("- beta: Second skill.") != std::string::npos);
}
