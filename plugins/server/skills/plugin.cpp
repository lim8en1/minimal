// skills -- SKILL.md instruction bundles, loaded on demand or preloaded.
//
//   register_tools              the `skill` tool (loads a skill's body)
//   register_permission_models  the per-skill gate on that tool (skills_model)
//   register_preset_extensions  per preset: the tool's description lists the
//                               skills that preset may load (or the tool is
//                               withheld when it may load none), and preloaded
//                               bodies join its system prompt
//   describe_project_content    a project's skills/ (and .claude/.agents
//                               overlays) for the trust prompt
//
// Skills live in <config dir>/skills/<name>/SKILL.md, plus a trusted project's
// own skills/, .claude/skills/ and .agents/skills/. Which skills a preset gets
// is its `plugin_permissions.skills` slice over permissions.json's
// `plugins.skills` (skills_model.hpp). Disabled, no skill tool exists and
// nothing about skills reaches a prompt.

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/config/project_dirs.hpp"
#include "minimal/config/project_trust.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/plugins/service_registry.hpp"

#include "skill_registry.hpp"
#include "skill_tool.hpp"
#include "skills_model.hpp"
#include "skills_preset.hpp"

namespace {

using minimal::skills::SkillRegistry;

// Process lifetime, like the plugin itself (never unloaded): every object
// registered below holds a reference into it.
std::unique_ptr<SkillRegistry>& Registry() {
    static std::unique_ptr<SkillRegistry> registry;
    return registry;
}

SkillRegistry& EnsureRegistry(const minimal::plugins::PluginRegistrationContext& context) {
    auto& registry = Registry();
    if (!registry) {
        registry = std::make_unique<SkillRegistry>(
            SkillRegistry::Load(minimal::common::ConfigDir()));
        if (context.services != nullptr) {
            if (void* trust = context.services->Find(minimal::config::kProjectTrustServiceName)) {
                registry->SetProjectTrust(static_cast<minimal::config::ProjectTrustQuery*>(trust));
            }
        }
        auto& log = minimal::common::DefaultLogger();
        if (!registry->scan_error().empty()) {
            log.warn("skills: {}", registry->scan_error());
        }
        for (const auto& meta : registry->All()) {
            if (!meta.loaded) log.warn("skill '{}' not loaded: {}", meta.name, meta.load_error);
        }
        log.info("skills: {} found", registry->All().size());
    }
    return *registry;
}

std::size_t CountSkills(const std::filesystem::path& dir) {
    std::error_code ec;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_directory(entry_ec) &&
            std::filesystem::exists(it->path() / "SKILL.md", entry_ec)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("skills")

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(std::make_unique<minimal::skills::SkillTool>(EnsureRegistry(context)));
}

void minimal_plugin_register_permission_models(
    minimal::tools::PermissionModelRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    registry.Register(minimal::skills::MakeSkillsPermissionModel(&EnsureRegistry(context)));
}

void minimal_plugin_register_preset_extensions(
    minimal::agent::PresetExtensionRegistry& extensions,
    const minimal::plugins::PluginRegistrationContext& context) {
    const SkillRegistry& registry = EnsureRegistry(context);
    extensions.AddDefinitionBuilder(minimal::skills::kSkillToolName,
                                    minimal::skills::MakeSkillDefinitionBuilder(registry));
    extensions.AddPromptAppendix(minimal::skills::MakePreloadAppendix(registry));
}

void minimal_plugin_describe_project_content(
    const std::filesystem::path& project_root,
    std::vector<minimal::config::ProjectContentCount>& out) {
    std::size_t count = 0;
    // Named: dirs() returns a reference into the ProjectDirs, which a
    // temporary in the range-for would not keep alive.
    const minimal::config::ProjectDirs project(project_root);
    for (const auto& dir : project.dirs()) {
        for (const char* sub : {"skills", ".claude/skills", ".agents/skills"}) {
            count += CountSkills(dir / sub);
        }
    }
    out.push_back({"skill", "skills", count});
}

}  // extern "C"
