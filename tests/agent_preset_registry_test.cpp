#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <string>

#include <unistd.h>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/agent/spawn_subagent_tool.hpp"
#include "minimal/agent/subagent_spawn.hpp"
#include "minimal/mcp/mcp_permission_model.hpp"
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
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const AgentPreset& preset = registry.Resolve("build");
    CHECK(preset.name == "build");
    CHECK(preset.provider.base_url == "https://api.openai.com/v1");
    CHECK(preset.provider.model_name == "gpt-4o-mini");
    CHECK_FALSE(preset.provider.api_key.configured());
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
    AgentPresetRegistry registry(config.dir,
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
    AgentPresetRegistry registry(config.dir,
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
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").provider.base_url == "https://api.openai.com/v1");
}

TEST_CASE("AgentPresetRegistry: model_declared records whether the preset named its model",
          "[agent_preset_registry]") {
    // Load-bearing for the spawn path's "inherit the parent's model" default:
    // a declared `model:` must survive it, a settings default must not be
    // mistaken for one.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "---\n"
                  "body\n");
    config.WriteJson("settings.json", {{"default_model", "openai-main/gpt-4o-mini"}});
    config.Write("agents/other.md",
                  "---\n"
                  "name: other\n"
                  "model: openai-main/gpt-4.1\n"
                  "---\n"
                  "other body\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    // Resolved from settings.default_model: not a declaration.
    const auto fallback = registry.Resolve("build");
    CHECK(fallback.provider.model_name == "gpt-4o-mini");
    CHECK_FALSE(fallback.model_declared);

    // Resolved from the preset's own `model:`: declared.
    const auto declared = registry.Resolve("other");
    CHECK(declared.provider.model_name == "gpt-4.1");
    CHECK(declared.model_declared);

    // A model_override stamps the model but does not invent a declaration:
    // the caller chose the model, the preset still did not.
    const auto overridden = registry.Resolve(
        "build", {}, nullptr, common::SplitModelRef("openai-main/gpt-4.1"));
    CHECK(overridden.provider.model_name == "gpt-4.1");
    CHECK_FALSE(overridden.model_declared);
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

    auto passed = config::LoadSettingsConfig(config.dir / "settings.json");
    passed.default_model = "openai-main/gpt-4.1";
    AgentPresetRegistry registry(config.dir, passed);

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
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK_THROWS_AS(registry.Resolve("build"), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: a model_override rescues a preset with no model and no "
          "default_model",
          "[agent_preset_registry]") {
    // THE BUG this guards against: settings.json default_model was junk (or, as
    // here, simply absent) and every preset declared no model of its own, so
    // Resolve() threw before an explicit --model/--model override the caller
    // already validated ever got a chance to apply. A caller-supplied override
    // must resolve the session regardless of what the preset or settings.json
    // says.
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("settings.json", nlohmann::json::object());
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "---\n"
                  "body\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const AgentPreset preset = registry.Resolve(
        "build", /*project_root=*/{}, /*parent=*/nullptr,
        config::ModelRef{"openai-main", "gpt-4.1"});
    CHECK(preset.provider.model_name == "gpt-4.1");
}

TEST_CASE("AgentPresetRegistry: a model_override rescues a preset whose OWN model is "
          "unresolvable",
          "[agent_preset_registry]") {
    // Same rescue, but the preset names a real (broken) model rather than none
    // at all -- the override must win outright, not merely fill a gap.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: nonexistent/model\n"
                  "---\n"
                  "body\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const AgentPreset preset = registry.Resolve(
        "build", /*project_root=*/{}, /*parent=*/nullptr,
        config::ModelRef{"openai-main", "small"});
    CHECK(preset.provider.model_name == "small");
}

TEST_CASE("AgentPresetRegistry: an unresolvable model_override still throws, named as the "
          "override",
          "[agent_preset_registry]") {
    // The rescue must not silently fall back to a broken preset/default_model
    // when the override ITSELF is bad -- the user asked for a specific model,
    // and getting a different one with no warning would be worse than an error.
    TempConfigDir config;
    config.WriteDefaults();
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    try {
        registry.Resolve("build", /*project_root=*/{}, /*parent=*/nullptr,
                         config::ModelRef{"openai-main", "nonexistent"});
        FAIL("expected an unresolvable override to throw");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find("openai-main/nonexistent") != std::string::npos);
    }
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
    AgentPresetRegistry registry(config.dir,
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
    auto catalog = config::LoadProvidersConfig(config.dir / "providers.json");
    AgentPresetRegistry registry(config.dir,
                                 config::LoadSettingsConfig(config.dir / "settings.json"),
                                 config::LoadPermissionsConfig(config.dir / "permissions.json"),
                                 catalog);

    int calls = 0;
    catalog.SetDiscoveryHook(
        [&calls](const std::string& kind, const llm::ProviderConfig& c) {
            ++calls;
            CHECK(kind == "openai");
            CHECK(c.base_url == "http://localhost:8000/v1");
            return config::DiscoveryOutcome{
                /*catalog_present=*/true, /*ok=*/true, {},
                {llm::DiscoveredModel{"served-model", 262144, nlohmann::json::object(),
                                      std::nullopt}}};
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
    AgentPresetRegistry registry(config.dir,
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
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto& policy = registry.Resolve("build").permission_mode;
    // preset per-tool override beats everything, including global per-tool.
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysDeny);
    // preset category beats global fallback for a tool with no per-tool rule.
    CHECK(policy.mode_for("other", ToolCategory::Execute) == PermissionMode::Ask);
}

TEST_CASE("AgentPresetRegistry: the 4-arg overload uses the injected PermissionsConfig, "
          "not permissions.json on disk",
          "[agent_preset_registry]") {
    // Phase 3: main.cpp passes the sanity check's already-parsed
    // PermissionsConfig instead of letting this class re-read the file.
    // permissions.json on disk says "deny"; the injected config says
    // "allow" -- the resolved policy must reflect the injection, proving no
    // reload happened.
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json", {{"fallback", {{"execute", "deny"}}}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "---\n"
                  "body\n");
    const auto injected = config::ParsePermissionsConfig(
        nlohmann::json{{"fallback", {{"execute", "allow"}}}});
    AgentPresetRegistry registry(config.dir,
                                 config::LoadSettingsConfig(config.dir / "settings.json"),
                                 injected);

    const auto& policy = registry.Resolve("build").permission_mode;
    CHECK(policy.mode_for("bash", ToolCategory::Execute) == PermissionMode::AlwaysAllow);
}

TEST_CASE("AgentPresetRegistry: global per-tool override applies when preset is silent",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json", {{"per_tool_overrides", {{"bash", "allow"}}}});
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").permission_mode.mode_for("bash", ToolCategory::Execute) ==
          PermissionMode::AlwaysAllow);
}

TEST_CASE("AgentPresetRegistry: falls to hard-coded default with no config at all", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").permission_mode.mode_for("anything", ToolCategory::Execute) ==
          PermissionMode::Ask);
}

TEST_CASE("AgentPresetRegistry: MCP slices bind through the mcp plugin's model",
          "[agent_preset_registry][permission_model]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json", {{"plugins", {{"mcp", {{"default", "deny"}}}}}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "plugin_permissions:\n"
                  "  mcp:\n"
                  "    servers:\n"
                  "      search: allow\n"
                  "---\n"
                  "body\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    tools::PermissionModelRegistry models;
    models.SetAttribution("mcp");
    models.Register(mcp::MakeMcpPermissionModel());
    models.ClearAttribution();
    registry.SetPermissionModels(&models);

    const auto& bound = registry.Resolve("build").permission_models;
    REQUIRE(bound.size() == 1);
    const auto& policy = *static_cast<const mcp::McpPermissionPolicy*>(bound.front().layer.get());
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysAllow);
    CHECK(policy.mode_for("unconfigured") == PermissionMode::AlwaysDeny);
}

TEST_CASE("AgentPresetRegistry: plugin permission models bind both slices",
          "[agent_preset_registry][permission_model]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.WriteJson("permissions.json",
                     {{"plugins", {{"mcp-client", {{"default", "deny"}}}}}});
    config.Write("agents/build.md",
                  "---\n"
                  "name: build\n"
                  "model: openai-main/gpt-4o-mini\n"
                  "plugin_permissions:\n"
                  "  mcp-client:\n"
                  "    servers:\n"
                  "      search: allow\n"
                  "---\n"
                  "body\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    // Any registered model binds; the MCP model is a real one whose slices
    // are easy to observe.
    tools::PermissionModelRegistry models;
    models.SetAttribution("mcp-client");
    models.Register(mcp::MakeMcpPermissionModel());
    models.ClearAttribution();
    registry.SetPermissionModels(&models);

    const auto& bound = registry.Resolve("build").permission_models;
    REQUIRE(bound.size() == 1);
    CHECK(bound.front().name == "mcp-client");
    const auto& policy = *static_cast<const mcp::McpPermissionPolicy*>(bound.front().layer.get());
    CHECK(policy.mode_for("search") == PermissionMode::AlwaysAllow);
    CHECK(policy.mode_for("unconfigured") == PermissionMode::AlwaysDeny);
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
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").compression_prompt == "Custom compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: compression_prompt refuses path traversal",
          "[agent_preset_registry]") {
    // The frontmatter string becomes a path into prompts/. Repo-shipped
    // presets are untrusted content: a traversal or absolute name must not
    // read arbitrary *.md into the summarization prompt.
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/custom.md", "Custom compaction prompt.\n");
    config.Write("prompts/evil.md", "Evil compaction prompt.\n");

    for (const auto* bad : {"../../prompts/evil", "/etc/evil", "custom\\evil", "..", "."}) {
        config.Write("agents/build.md",
                     std::string("---\n"
                                 "name: build\n"
                                 "model: openai-main/gpt-4o-mini\n"
                                 "compression_prompt: ") +
                         bad + "\n---\nbody\n");
        AgentPresetRegistry registry(config.dir,
                                     config::LoadSettingsConfig(config.dir / "settings.json"));
        CHECK_THROWS(registry.Resolve("build"));
    }
    // The honest prompt file still resolves; nothing else was disturbed.
    config.Write("agents/build.md",
                 "---\n"
                 "name: build\n"
                 "model: openai-main/gpt-4o-mini\n"
                 "compression_prompt: custom\n"
                 "---\n"
                 "body\n");
    AgentPresetRegistry registry(config.dir,
                                 config::LoadSettingsConfig(config.dir / "settings.json"));
    CHECK(registry.Resolve("build").compression_prompt == "Custom compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: compression_prompt falls back to prompts/compression.md when omitted",
          "[agent_preset_registry]") {    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/compression.md", "Default compaction prompt.\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("build").compression_prompt == "Default compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: missing default compression prompt yields empty string, no throw",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    AgentPresetRegistry registry(config.dir,
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
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK_THROWS_AS(registry.Resolve("build"), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: ResolveCompressionPrompt loads the named file's body",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/terse.md", "---\nignored: yes\n---\nTerse compaction prompt.\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.ResolveCompressionPrompt("terse") == "Terse compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: ResolveCompressionPrompt applies the same bare-filename rule",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/terse.md", "Terse compaction prompt.\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    // Same traversal shapes the frontmatter field refuses: the /compact <name>
    // argument builds a path into prompts/ just the same way.
    for (const auto* bad : {"../../prompts/evil", "/etc/evil", "terse\\evil", "..", "."}) {
        CHECK_THROWS_AS(registry.ResolveCompressionPrompt(bad), std::runtime_error);
    }
    CHECK_THROWS_AS(registry.ResolveCompressionPrompt("nonexistent"), std::runtime_error);
    // The honest name still resolves after all the failures.
    CHECK(registry.ResolveCompressionPrompt("terse") == "Terse compaction prompt.\n");
}

TEST_CASE("AgentPresetRegistry: missing prompts/subagent.md yields an empty SubagentPrefix", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.SubagentPrefix().empty());
}

TEST_CASE("AgentPresetRegistry: present prompts/subagent.md is read verbatim", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("prompts/subagent.md", "You are in subagent mode.\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.SubagentPrefix() == "You are in subagent mode.\n");
}

TEST_CASE("AgentPresetRegistry: malformed providers.json throws at construction", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("providers.json", "{not valid json");

    CHECK_THROWS_AS(AgentPresetRegistry(config.dir,
                                     config::LoadSettingsConfig(config.dir / "settings.json")),
                     std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: malformed permissions.json throws at construction", "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("permissions.json", "{not valid json");

    CHECK_THROWS_AS(AgentPresetRegistry(config.dir,
                                     config::LoadSettingsConfig(config.dir / "settings.json")),
                     std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: Resolve() caches -- a second call survives the file being deleted",
          "[agent_preset_registry]") {
    TempConfigDir config;
    config.WriteDefaults();
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const std::string first_name = registry.Resolve("build").name;
    std::filesystem::remove(config.dir / "agents" / "build.md");

    CHECK(registry.Resolve("build").name == first_name);
}

TEST_CASE("AgentPresetRegistry: ListAvailable lists every agents/*.md by name, sorted",
          "[agent_preset_registry][list]") {
    TempConfigDir config;
    config.WriteDefaults();  // agents/build.md, name "build"
    config.Write("agents/plan.md",
                 "---\nname: plan\ndescription: Plans work.\nmodel: openai-main/gpt-4o-mini\n---\nbody\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto summaries = registry.ListAvailable();
    REQUIRE(summaries.size() == 2);
    CHECK(summaries[0].name == "build");
    CHECK(summaries[1].name == "plan");
    CHECK(summaries[1].description == "Plans work.");
}

TEST_CASE("AgentPresetRegistry: ListAvailable keys on the filename, not the frontmatter "
          "display name, so every listed entry is selectable",
          "[agent_preset_registry][list][invariant]") {
    // THE BUG: Resolve()/ResolveUncached() look a preset up by FILENAME
    // (agents/<name>.md), never by the frontmatter `name:` field -- but
    // ListAvailable used to key its summaries on raw.name. A preset whose two
    // diverge (a real, encouraged case: "Builder" reads better in a picker
    // than "build") was listed under a name Resolve() could never find --
    // picking it from the client would fail with "unknown agent preset".
    TempConfigDir config;
    config.WriteDefaults();  // agents/build.md, name "build"
    config.Write("agents/senior-reviewer.md",
                 "---\nname: Senior Reviewer\ndescription: Reviews PRs.\nmodel: "
                 "openai-main/gpt-4o-mini\n---\nbody\n");
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto summaries = registry.ListAvailable();
    REQUIRE(summaries.size() == 2);
    const auto it = std::find_if(summaries.begin(), summaries.end(), [](const auto& summary) {
        return summary.description == "Reviews PRs.";
    });
    REQUIRE(it != summaries.end());
    // The listed name is the FILE STEM ("senior-reviewer"), not the
    // frontmatter display value ("Senior Reviewer") -- and Resolve() of
    // exactly that string succeeds.
    CHECK(it->name == "senior-reviewer");
    CHECK_NOTHROW(registry.Resolve(it->name));
}

TEST_CASE("AgentPresetRegistry: ListAvailable does not resolve a model, so a bad provider "
          "does not break the listing",
          "[agent_preset_registry][list]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/broken.md",
                 "---\nname: broken\nmodel: nonexistent/model\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto summaries = registry.ListAvailable();
    REQUIRE(summaries.size() == 2);
    CHECK(summaries[0].name == "broken");
    CHECK(summaries[1].name == "build");
}

TEST_CASE("AgentPresetRegistry: ListAvailable skips a file with no parseable frontmatter",
          "[agent_preset_registry][list]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/malformed.md", "not frontmatter at all\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto summaries = registry.ListAvailable();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].name == "build");
}

TEST_CASE("AgentPresetRegistry: ListAvailable on an empty agents/ dir returns empty",
          "[agent_preset_registry][list]") {
    TempConfigDir config;
    config.WriteJson("providers.json", TempConfigDir::DefaultProviders());
    config.WriteJson("settings.json", nlohmann::json::object());
    config.WriteJson("permissions.json", nlohmann::json::object());

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.ListAvailable().empty());
}

TEST_CASE("AgentPresetRegistry: allowed_subagents resolves -- absent and '*' unrestricted, "
          "a list verbatim, empty stays empty",
          "[agent_preset_registry][subagents]") {
    TempConfigDir config;
    config.WriteDefaults();
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto resolved = [&](const std::string& key, const std::string& frontmatter) {
        config.Write("agents/" + key + ".md",
                     "---\nname: " + key + "\nmodel: openai-main/gpt-4o-mini\n" + frontmatter +
                         "---\nbody\n");
        return registry.Resolve(key).allowed_subagents;
    };

    // Distinct keys per case, not rewrites of one file: Resolve caches per
    // (name, root), so a second resolve of a rewritten preset returns the
    // first answer.
    CHECK_FALSE(resolved("default", "").has_value());                  // the default: everything
    CHECK_FALSE(resolved("starred", "allowed_subagents: [\"*\"]\n").has_value());  // explicit all
    CHECK(*resolved("listed", "allowed_subagents: [plan, build]\n") ==
          std::vector<std::string>{"plan", "build"});
    const auto empty = resolved("none", "allowed_subagents: []\n");    // explicit none
    REQUIRE(empty.has_value());
    CHECK(empty->empty());
}

TEST_CASE("AgentPresetRegistry: the spawn definition builder names the default, lists "
          "spawnable presets (hidden included), and enums the allowlist",
          "[agent_preset_registry][subagents]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/researcher.md",
                 "---\nname: researcher\ndescription: Deep research.\nmodel: openai-main/gpt-4.1\n---\nbody\n");
    // hidden and mode:subagent presets are NOT picker-listed but ARE spawnable:
    // the spawn listing must include them even though ListAvailable excludes them.
    config.Write("agents/secret.md",
                 "---\nname: secret\ndescription: Hidden worker.\nmodel: openai-main/gpt-4.1\nhidden: true\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    registry.SetDefinitionBuilder(kSpawnSubagentToolName, MakeSpawnDefinitionBuilder());

    const auto preset = registry.Resolve("build");
    REQUIRE(preset.tool_definition_overrides.count("spawn_subagent") == 1);
    const auto& def = preset.tool_definition_overrides.at("spawn_subagent");

    // The default line names the preset KEY and the preset's own description
    // (absent here, so no parenthetical).
    CHECK(def.description.find("Default subagent preset: `build`") != std::string::npos);
    CHECK(def.description.find("(You are a build agent.)") == std::string::npos);
    // Spawnable listing: researcher AND the hidden preset, sorted.
    CHECK(def.description.find("- `researcher` (Deep research.)") != std::string::npos);
    CHECK(def.description.find("- `secret` (Hidden worker.)") != std::string::npos);
    // The enum carries exactly the names the schema will accept.
    REQUIRE(def.input_schema["properties"]["agent_preset"].contains("enum"));
    CHECK(def.input_schema["properties"]["agent_preset"]["enum"] ==
          nlohmann::json::array({"build", "researcher", "secret"}));
}

TEST_CASE("AgentPresetRegistry: the spawn builder filters by allowed_subagents and omits "
          "a default line the enforcement would refuse",
          "[agent_preset_registry][subagents]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/researcher.md",
                 "---\nname: researcher\ndescription: Deep research.\nmodel: openai-main/gpt-4.1\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    registry.SetDefinitionBuilder(kSpawnSubagentToolName, MakeSpawnDefinitionBuilder());

    SECTION("the allowlist filters both the listing and the enum") {
        config.Write("agents/build.md",
                     "---\nname: build\nmodel: openai-main/gpt-4o-mini\n"
                     "allowed_subagents: [researcher]\n---\nYou are a build agent.\n");
        const auto def = registry.Resolve("build").tool_definition_overrides.at("spawn_subagent");
        CHECK(def.description.find("- `researcher` (Deep research.)") != std::string::npos);
        CHECK(def.description.find("`build`") == std::string::npos);
        CHECK(def.input_schema["properties"]["agent_preset"]["enum"] ==
              nlohmann::json::array({"researcher"}));
    }

    SECTION("own preset not allowlisted: no default line is advertised") {
        config.Write("agents/build.md",
                     "---\nname: build\nmodel: openai-main/gpt-4o-mini\n"
                     "allowed_subagents: [researcher]\n---\nbody\n");
        // build is not in its own allowlist, so "Defaults to your own" would
        // advertise a spawn the manager refuses; the line must be gone while
        // the listing itself survives.
        const auto def = registry.Resolve("build").tool_definition_overrides.at("spawn_subagent");
        CHECK(def.description.find("Default subagent preset") == std::string::npos);
        CHECK(def.description.find("- `researcher` (Deep research.)") != std::string::npos);
    }

    SECTION("an empty allowlist yields no listing and no enum") {
        config.Write("agents/build.md",
                     "---\nname: build\nmodel: openai-main/gpt-4o-mini\n"
                     "allowed_subagents: []\n---\nbody\n");
        const auto def = registry.Resolve("build").tool_definition_overrides.at("spawn_subagent");
        CHECK(def.description.find("Available presets") == std::string::npos);
        CHECK_FALSE(def.input_schema["properties"]["agent_preset"].contains("enum"));
    }
}

// --- project-local discovery ---------------------------------------------------

TEST_CASE("AgentPresetRegistry: a project preset shadows a global one of the same name",
          "[agent_preset_registry][project]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4.1\n---\nPROJECT BUILD.\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    const auto project_root = config.dir / "proj";

    const auto preset = registry.Resolve("build", project_root);
    CHECK(preset.system_prompt == "PROJECT BUILD.\n");
    // The project frontmatter won: gpt-4.1, not the global preset's gpt-4o-mini.
    CHECK(preset.model_ref.str() == "openai-main/gpt-4.1");

    // The global view is one Resolve away, unchanged: an empty root (or a
    // different root) reads the global file.
    CHECK(registry.Resolve("build").system_prompt == "You are a build agent.\n");
}

TEST_CASE("AgentPresetRegistry: a project preset can narrow permissions but never widen them",
          "[agent_preset_registry][project][permissions]") {
    TempConfigDir config;
    config.WriteDefaults();  // empty permissions.json: global falls to hard-coded defaults
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    // A hostile repo shipping this would, before the clamp, silently replace
    // the operator's Ask floor on bash/write/network with AlwaysAllow the
    // moment a session opened in this directory -- no prompt, no diff.
    config.Write("proj/agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4o-mini\n"
                 "permission:\n"
                 "  bash: allow\n"
                 "  edit: allow\n"
                 "  external_directory: allow\n"
                 "  webfetch: allow\n"
                 "  read: deny\n"  // AlwaysDeny -- narrowing, must survive the clamp
                 "---\nProject preset.\n");
    // The operator's OWN global preset, with the identical override: fully
    // trusted, must resolve exactly as written (contrast case).
    config.Write("agents/trusted.md",
                 "---\nname: trusted\nmodel: openai-main/gpt-4o-mini\n"
                 "permission:\n  bash: allow\n---\nGlobal preset.\n");

    // The live tool vocabulary the `permission:` block resolves against.
    ToolPermissionKeys tools{{"bash", "read", "edit", "web_fetch"}, {}, {{"webfetch", "web_fetch"}},
                             {"web_fetch"}};
    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"), tools);

    const auto project_preset = registry.Resolve("build", config.dir / "proj");
    // Clamped to the hard-coded default (Ask), never AlwaysAllow: the project
    // preset narrowed nothing it named, so most_restrictive(Allow, Ask) = Ask.
    CHECK(project_preset.permission_mode.mode_for("bash", ToolCategory::Execute) ==
          PermissionMode::Ask);
    CHECK(project_preset.permission_mode.mode_for("anyfile", ToolCategory::FileWrite) ==
          PermissionMode::Ask);
    CHECK(project_preset.permission_mode.out_of_workdir == PermissionMode::Ask);
    CHECK(project_preset.permission_mode.network == PermissionMode::Ask);
    // A narrowing per-tool override (AlwaysDeny) still applies from an
    // untrusted preset -- clamp-only means it can only get MORE restrictive.
    CHECK(project_preset.permission_mode.mode_for("read", ToolCategory::ReadOnly) ==
          PermissionMode::AlwaysDeny);

    // The operator's own global preset is unaffected: it stays fully trusted.
    const auto trusted_preset = registry.Resolve("trusted");
    CHECK(trusted_preset.permission_mode.mode_for("bash", ToolCategory::Execute) ==
          PermissionMode::AlwaysAllow);
}

TEST_CASE("AgentPresetRegistry: a project-only preset resolves; a project root cannot "
          "see another project's presets",
          "[agent_preset_registry][project]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/agents/local.md",
                 "---\nname: local\nmodel: openai-main/gpt-4o-mini\n---\nLOCAL.\n");
    config.Write("other/.git/HEAD", "ref: refs/heads/main\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    CHECK(registry.Resolve("local", config.dir / "proj").system_prompt == "LOCAL.\n");
    // Unknown to the GLOBAL config and to the asking project: the same
    // "unknown agent preset" error a global-only resolve throws.
    CHECK_THROWS_AS(registry.Resolve("local", config.dir / "other"), std::runtime_error);
    CHECK_THROWS_AS(registry.Resolve("local"), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: ListAvailable merges project presets, project shadows global",
          "[agent_preset_registry][project][list]") {
    TempConfigDir config;
    config.WriteDefaults();  // global "build"
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/agents/build.md",
                 "---\nname: build\ndescription: Project variant.\n---\nbody\n");
    config.Write("proj/agents/local.md",
                 "---\nname: local\ndescription: Local only.\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    const auto project_root = config.dir / "proj";

    const auto summaries = registry.ListAvailable(project_root);
    REQUIRE(summaries.size() == 2);  // local + ONE build
    CHECK(summaries[0].name == "build");
    CHECK(summaries[0].description == "Project variant.");
    CHECK(summaries[1].name == "local");

    // No project scope: the global listing stands.
    const auto global = registry.ListAvailable();
    REQUIRE(global.size() == 1);
    CHECK(global.front().name == "build");
    CHECK(global.front().description.empty());  // global build.md has no description
}

TEST_CASE("AgentPresetRegistry: an invisible project preset still shadows a global of the "
          "same name in the listing",
          "[agent_preset_registry][project][list]") {
    // A project `hidden` preset is spawnable but not listed -- yet Resolve()
    // still picks the PROJECT file for that name. The listing must therefore
    // not resurrect the global preset under the same name: the picker would
    // offer a preset no lookup can reach.
    TempConfigDir config;
    config.WriteDefaults();  // global "build"
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4o-mini\nhidden: true\n"
                 "description: hidden variant.\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));
    const auto project_root = config.dir / "proj";

    const auto summaries = registry.ListAvailable(project_root);
    CHECK(summaries.empty());

    // Spawn still reaches the project file (hidden presets are spawnable).
    CHECK(registry.Resolve("build", project_root).system_prompt == "body\n");

    // Without project scope the global build stays listed.
    CHECK(registry.ListAvailable().size() == 1);
}

// --- opencode-compat preset surface -----------------------------------------------

TEST_CASE("AgentPresetRegistry: a disabled preset refuses to resolve and is not listed",
          "[agent_preset_registry][compat]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/gone.md",
                 "---\nname: gone\nmodel: openai-main/gpt-4o-mini\ndisable: true\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    bool threw_disabled = false;
    try {
        registry.Resolve("gone");
    } catch (const std::exception& e) {
        threw_disabled = std::string(e.what()).find("disabled") != std::string::npos;
    }
    CHECK(threw_disabled);
    for (const auto& summary : registry.ListAvailable()) {
        CHECK(summary.name != "gone");
    }
}

TEST_CASE("AgentPresetRegistry: hidden and subagent-mode presets stay resolvable but are "
          "not listed",
          "[agent_preset_registry][compat]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/scout.md",
                 "---\nname: scout\nmodel: openai-main/gpt-4o-mini\nmode: subagent\n---\nbody\n");
    config.Write("agents/secret.md",
                 "---\nname: secret\nmodel: openai-main/gpt-4o-mini\nhidden: true\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    for (const auto& summary : registry.ListAvailable()) {
        CHECK(summary.name != "scout");
        CHECK(summary.name != "secret");
    }
    // Visibility-only: both resolve, and both remain spawnable whatever the
    // mode says (mode never gates spawnability).
    CHECK(registry.Resolve("scout").system_prompt == "body\n");
    CHECK(registry.Resolve("secret").system_prompt == "body\n");
}

TEST_CASE("AgentPresetRegistry: frontmatter parameter passthrough overrides the model config",
          "[agent_preset_registry][compat]") {
    TempConfigDir config;
    config.WriteDefaults();  // provider-level parameters: {"temperature": 0.5}
    config.Write("agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4o-mini\ntemperature: 0.1\n"
                 "reasoningEffort: high\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto& parameters = registry.Resolve("build").parameters;
    CHECK(parameters["temperature"] == 0.1);  // the preset's extra beats the model's 0.5
    CHECK(parameters["reasoningEffort"] == "high");
}

TEST_CASE("AgentPresetRegistry: the listing carries the preset color",
          "[agent_preset_registry][compat]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4o-mini\ncolor: \"#ff6b6b\"\n---\nbody\n");

    AgentPresetRegistry registry(config.dir,
                                  config::LoadSettingsConfig(config.dir / "settings.json"));

    const auto summaries = registry.ListAvailable();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries.front().color == "#ff6b6b");
}

// --- the project trust gate ---------------------------------------------------

TEST_CASE("AgentPresetRegistry: an untrusted project's presets are invisible until trusted",
          "[agent_preset_registry][project][trust]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/agents/local.md",
                 "---\nname: local\nmodel: openai-main/gpt-4o-mini\n---\nLOCAL.\n");
    config.Write("proj/agents/build.md",
                 "---\nname: build\nmodel: openai-main/gpt-4.1\n---\nPROJECT BUILD.\n");
    const auto project_root = config.dir / "proj";

    config::ProjectTrust trust(config.dir / "trusted_projects.json",
                               config::ProjectLocalMode::kTrusted);
    AgentPresetRegistry registry(config.dir,
                                 config::LoadSettingsConfig(config.dir / "settings.json"));
    registry.SetProjectTrust(&trust);

    // Untrusted: the project-only preset is unknown, the shadowing one is the
    // GLOBAL build, and the picker lists neither project entry.
    CHECK_THROWS_AS(registry.Resolve("local", project_root), std::runtime_error);
    CHECK(registry.Resolve("build", project_root).system_prompt == "You are a build agent.\n");
    for (const auto& summary : registry.ListAvailable(project_root)) {
        CHECK(summary.name != "local");
    }
    CHECK(registry.Resolve("build", project_root).model_ref.str() != "openai-main/gpt-4.1");

    // Trusting takes effect on the next call, with no restart -- and the earlier
    // (untrusted) resolve did not poison the cache.
    std::string error;
    REQUIRE(trust.Trust(project_root, error));
    CHECK(registry.Resolve("local", project_root).system_prompt == "LOCAL.\n");
    CHECK(registry.Resolve("build", project_root).system_prompt == "PROJECT BUILD.\n");

    // Revoking hides them again even though they were cached while trusted.
    REQUIRE(trust.Revoke(project_root, error));
    CHECK_THROWS_AS(registry.Resolve("local", project_root), std::runtime_error);
    CHECK(registry.Resolve("build", project_root).system_prompt == "You are a build agent.\n");
}

TEST_CASE("AgentPresetRegistry: mode all loads project presets without a grant; none never does",
          "[agent_preset_registry][project][trust]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    config.Write("proj/agents/local.md",
                 "---\nname: local\nmodel: openai-main/gpt-4o-mini\n---\nLOCAL.\n");
    const auto project_root = config.dir / "proj";
    AgentPresetRegistry registry(config.dir,
                                 config::LoadSettingsConfig(config.dir / "settings.json"));

    config::ProjectTrust all(config.dir / "a.json", config::ProjectLocalMode::kAll);
    registry.SetProjectTrust(&all);
    CHECK(registry.Resolve("local", project_root).system_prompt == "LOCAL.\n");

    config::ProjectTrust none(config.dir / "b.json", config::ProjectLocalMode::kNone);
    std::string error;
    REQUIRE(none.Trust(project_root, error));  // a grant does not beat "none"
    registry.SetProjectTrust(&none);
    CHECK_THROWS_AS(registry.Resolve("local", project_root), std::runtime_error);
}

TEST_CASE("AgentPresetRegistry: plugins: expands to the plugins' tools beside allowed_tools",
          "[agent_preset_registry][plugins]") {
    TempConfigDir config;
    config.WriteDefaults();
    config.Write("agents/grouped.md",
                 "---\nname: grouped\nmodel: openai-main/gpt-4o-mini\n"
                 "allowed_tools: [ask_user, memory_get]\n"
                 "plugins: [memory, tools-fs, not-loaded]\n---\nGrouped.\n");

    ToolPermissionKeys tools;
    tools.all = {"ask_user", "edit", "memory_get", "memory_search", "memory_store", "read"};
    tools.by_plugin = {{"memory", {"memory_get", "memory_search", "memory_store"}},
                       {"tools-fs", {"edit", "read"}},
                       {"tool-ask", {"ask_user"}}};
    AgentPresetRegistry registry(config.dir,
                                 config::LoadSettingsConfig(config.dir / "settings.json"), tools);

    // allowed_tools first, verbatim; each group's keys after, deduped (memory_get
    // is named both ways); a plugin that is not loaded contributes nothing.
    CHECK(registry.Resolve("grouped").allowed_tools ==
          std::vector<std::string>{"ask_user", "memory_get", "memory_search", "memory_store",
                                   "edit", "read"});
    // A preset without plugins: is untouched.
    CHECK(registry.Resolve("build").allowed_tools.empty());
}

TEST_CASE("AgentPresetRegistry: a subagent's plugins: group is still clamped by its parent",
          "[agent_preset_registry][plugins]") {
    // Groups expand at resolve, so the spawn-time intersection sees two plain
    // lists: a child naming the whole memory group under a parent that allows
    // only memory_search gets memory_search alone.
    const std::vector<std::string> parent = {"read", "memory_search"};
    const std::vector<std::string> child_expanded = {"memory_get", "memory_search",
                                                     "memory_store"};
    CHECK(intersect_allowed(parent, child_expanded) ==
          std::vector<std::string>{"memory_search"});
}
