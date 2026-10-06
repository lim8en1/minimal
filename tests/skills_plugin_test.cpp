// The skills plugin's preset seams, end to end through AgentPresetRegistry:
// its permission model bound to each preset's `plugin_permissions.skills`
// slice, the per-preset `skill` tool definition, and preloaded bodies in the
// system prompt -- with the subagent clamp against the parent's preset.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "minimal/agent/agent_preset_registry.hpp"
#include "minimal/config/project_trust.hpp"
#include "minimal/tools/permission_model.hpp"
#include "skill_registry.hpp"
#include "skill_tool.hpp"
#include "skills_model.hpp"
#include "skills_preset.hpp"
#include "temp_dir.hpp"

using namespace minimal;
using agent::AgentPresetRegistry;

namespace {

struct SkillsHarness {
    testing::TempDir tempdir;
    std::filesystem::path dir = tempdir.path();
    tools::PermissionModelRegistry models;
    agent::PresetExtensionRegistry extensions;
    std::unique_ptr<skills::SkillRegistry> skills;
    std::unique_ptr<AgentPresetRegistry> registry;

    SkillsHarness() {
        Write("providers.json",
              R"({"providers":{"p":{"backend":"openai","url":"http://x/v1","models":{"m":{}}}}})");
        Write("settings.json", R"({"default_agent_preset":"build","default_model":"p/m"})");
        Write("permissions.json", "{}");
        Write("skills/alpha/SKILL.md", "---\nname: alpha\ndescription: First skill.\n---\nAlpha body.\n");
        Write("skills/beta/SKILL.md", "---\nname: beta\ndescription: Second skill.\n---\nBeta body.\n");
        Write("skills/style/SKILL.md", "---\nname: style\ndescription: House style.\n---\nALWAYS USE TABS.\n");
        Write("skills/broken/SKILL.md", "---\nname: broken\n---\nBody.\n");  // no description
    }

    void Write(const std::filesystem::path& relative, const std::string& contents) const {
        const auto path = dir / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << contents;
    }

    void Preset(const std::string& name, const std::string& skills_slice) {
        Write("agents/" + name + ".md", "---\nname: " + name + "\nmodel: p/m\n" + skills_slice +
                                            "---\nYou are " + name + ".\n");
    }

    void Build() {
        skills = std::make_unique<skills::SkillRegistry>(skills::SkillRegistry::Load(dir));
        models.SetAttribution("skills");
        models.Register(skills::MakeSkillsPermissionModel());
        models.ClearAttribution();
        extensions.SetAttribution("skills");
        extensions.AddDefinitionBuilder(skills::kSkillToolName,
                                        skills::MakeSkillDefinitionBuilder(*skills));
        extensions.AddPromptAppendix(skills::MakePreloadAppendix(*skills));
        extensions.ClearAttribution();
        registry = std::make_unique<AgentPresetRegistry>(
            dir, config::LoadSettingsConfig(dir / "settings.json"));
        registry->SetPermissionModels(&models);
        registry->SetExtensions(&extensions);
    }

    std::string Listing(const agent::AgentPreset& preset) const {
        const auto it = preset.tool_definition_overrides.find("skill");
        return it == preset.tool_definition_overrides.end() ? std::string("<none>")
                                                            : it->second.description;
    }
};

}  // namespace

TEST_CASE("allowed skills render into the skill tool's per-preset definition, sorted",
          "[skills_plugin]") {
    SkillsHarness h;
    // beta FIRST on purpose: the listing comes out sorted (cached prefix).
    h.Preset("build", "plugin_permissions:\n  skills: [beta, alpha]\n");
    h.Build();
    const auto preset = h.registry->Resolve("build");
    CHECK(h.Listing(preset) == skills::MakeSkillToolDefinition().description +
                                   "\n\nAvailable skills:\n- alpha: First skill.\n- beta: Second skill.");
    // Listed skills are not in the system prompt.
    CHECK(preset.system_prompt.find("Available skills") == std::string::npos);
}

TEST_CASE("a preset that may load no usable skill gets no skill tool", "[skills_plugin]") {
    SkillsHarness h;
    // Unknown and not-loaded names resolve out; nothing usable remains.
    h.Preset("build", "plugin_permissions:\n  skills: [broken, typo]\n");
    h.Preset("plain", "");
    h.Build();
    CHECK(h.Listing(h.registry->Resolve("build")).empty());  // withheld (empty-named)
    CHECK(h.Listing(h.registry->Resolve("plain")).empty());
}

TEST_CASE("the wildcard lists every loaded skill; specific beats it; deny resolves out",
          "[skills_plugin]") {
    SkillsHarness h;
    h.Preset("build",
             "plugin_permissions:\n  skills:\n    \"*\": ask\n    alpha: allow\n    beta: deny\n");
    h.Build();
    const std::string listing = h.Listing(h.registry->Resolve("build"));
    CHECK(listing.find("- alpha: First skill.\n") != std::string::npos);
    CHECK(listing.find("- style: House style. (asks before loading)") != std::string::npos);
    CHECK(listing.find("beta") == std::string::npos);
    CHECK(listing.find("broken") == std::string::npos);
}

TEST_CASE("a preloaded skill ships its body in the system prompt and is not listed",
          "[skills_plugin]") {
    SkillsHarness h;
    h.Preset("build", "plugin_permissions:\n  skills:\n    style: preload\n    alpha: allow\n");
    h.Build();
    const auto preset = h.registry->Resolve("build");
    CHECK(preset.system_prompt.find("## Preloaded skills") != std::string::npos);
    CHECK(preset.system_prompt.find("### style") != std::string::npos);
    CHECK(preset.system_prompt.find("ALWAYS USE TABS.") != std::string::npos);
    CHECK(h.Listing(preset).find("style") == std::string::npos);
    CHECK(h.Listing(preset).find("- alpha:") != std::string::npos);
}

TEST_CASE("a subagent keeps only its parent's skills, at the stricter mode", "[skills_plugin]") {
    SkillsHarness h;
    h.Preset("build", "plugin_permissions:\n  skills:\n    alpha: ask\n    style: preload\n");
    h.Preset("child", "plugin_permissions:\n  skills:\n    \"*\": preload\n");
    h.Build();
    const auto parent = h.registry->Resolve("build");
    const auto child = h.registry->Resolve("child", {}, &parent);
    // alpha: parent ask beats child preload -> listed as ask, NOT preloaded.
    CHECK(h.Listing(child).find("- alpha: First skill. (asks before loading)") != std::string::npos);
    CHECK(child.system_prompt.find("Alpha body.") == std::string::npos);
    // style: both preload -> stays preloaded.
    CHECK(child.system_prompt.find("ALWAYS USE TABS.") != std::string::npos);
    // beta: the parent lacks it -> gone.
    CHECK(h.Listing(child).find("beta") == std::string::npos);
    CHECK(child.system_prompt.find("Beta body.") == std::string::npos);
}

TEST_CASE("a preset resolved for a trusted project advertises that project's skills",
          "[skills_plugin][project]") {
    SkillsHarness h;
    h.Preset("build", "plugin_permissions:\n  skills: [\"*\"]\n");
    h.Write("proj/.git/HEAD", "ref: refs/heads/main\n");
    h.Write("proj/skills/gamma/SKILL.md", "---\nname: gamma\ndescription: Project.\n---\nG.\n");
    h.Build();
    const auto project_root = h.dir / "proj";
    CHECK(h.Listing(h.registry->Resolve("build", project_root)).find("- gamma: Project.") !=
          std::string::npos);
    // Without project scope the project's skill is not advertised.
    CHECK(h.Listing(h.registry->Resolve("build")).find("gamma") == std::string::npos);
}
