#include "minimal/agent/agent_preset_registry.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/agent/permission_resolution.hpp"
#include "minimal/config/frontmatter.hpp"

namespace minimal::agent {

namespace {

std::string ReadFileOrEmpty(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string ReadFileRequired(const std::filesystem::path& path, const std::string& what) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error(what + " not found: " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Renders the allowed skills' frontmatter (name + description only) into the
// system prompt -- the entire resident cost of a skill library. Only LOADED
// skills appear: a failed skill's reason surfaces through SkillTool at
// invocation time, and unknown names in allowed_skills are skipped here (a
// future sanity_check makes them loud).
//
// All() is sorted by name, so the listing is deterministic -- it sits in the
// cached prefix and must not reorder turn to turn.
// allowed_skills ∩ loaded, with `*` expanded. Computed ONCE and used for both
// the listing and AgentPreset::allowed_skills, so the prompt cannot advertise a
// skill the tool would refuse, nor the tool be offered with nothing listed.
std::vector<std::string> ResolveAllowedSkills(const std::vector<std::string>& allowed_skills,
                                              const config::SkillRegistry& skills) {
    const bool allow_all =
        std::find(allowed_skills.begin(), allowed_skills.end(), "*") != allowed_skills.end();

    std::vector<std::string> resolved;
    for (const auto& meta : skills.All()) {
        if (!meta.loaded) continue;
        const bool in =
            allow_all || std::find(allowed_skills.begin(), allowed_skills.end(), meta.name) !=
                             allowed_skills.end();
        if (in) resolved.push_back(meta.name);
    }
    return resolved;  // All() is sorted, so this is too
}

std::string RenderSkillListing(const std::vector<std::string>& resolved,
                               const config::SkillRegistry& skills) {
    if (resolved.empty()) return {};

    std::ostringstream out;
    out << "\n## Available skills\n\n";
    out << "Load a skill's full instructions with the `skill` tool.\n";
    for (const auto& name : resolved) {
        const config::SkillMeta* meta = skills.Find(name);
        if (meta == nullptr) continue;  // unreachable: resolved came from All()
        out << "- " << meta->name << ": " << meta->description << "\n";
    }
    return out.str();
}

}  // namespace

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SkillRegistry& skills)
    : config_dir_(std::move(config_dir)),
      providers_(config::LoadProvidersConfig(config_dir_ / "providers.json")),
      settings_(config::LoadSettingsConfig(config_dir_ / "settings.json")),
      permissions_(config::LoadPermissionsConfig(config_dir_ / "permissions.json")),
      skills_(skills) {
    // Both optional at this phase -- a real deployment's absence is a future
    // sanity_check's job to flag, not this constructor's.
    subagent_prefix_ = ReadFileOrEmpty(config_dir_ / "agents" / "subagent.md");
    default_compression_prompt_ =
        config::parse_frontmatter_markdown(ReadFileOrEmpty(config_dir_ / "prompts" / "compression.md")).body;
}

const AgentPreset& AgentPresetRegistry::Resolve(const std::string& preset_name) const {
    if (auto it = cache_.find(preset_name); it != cache_.end()) return it->second;
    auto [it, inserted] = cache_.emplace(preset_name, ResolveUncached(preset_name));
    (void)inserted;
    return it->second;
}

AgentPreset AgentPresetRegistry::ResolveUncached(const std::string& preset_name) const {
    if (preset_name.empty() || preset_name == "." || preset_name == ".." ||
        preset_name.find('/') != std::string::npos) {
        throw std::runtime_error("invalid agent preset name: '" + preset_name + "'");
    }

    const std::filesystem::path preset_path = config_dir_ / "agents" / (preset_name + ".md");
    if (!std::filesystem::exists(preset_path)) {
        throw std::runtime_error("unknown agent preset: '" + preset_name + "'");
    }
    const AgentPresetFrontmatter raw = LoadAgentPresetFrontmatter(preset_path);

    const std::string provider_name = raw.provider == "default" ? settings_.default_provider : raw.provider;
    if (provider_name.empty()) {
        throw std::runtime_error("agent preset '" + preset_name +
                                  "' resolves provider \"default\" but settings.json has no default_provider");
    }
    const config::ProviderEntry* entry = providers_.Find(provider_name);
    if (!entry) {
        throw std::runtime_error("agent preset '" + preset_name + "' names unknown provider '" + provider_name + "'");
    }

    AgentPreset preset;
    preset.name = raw.name;
    preset.description = raw.description;
    // The skill listing is part of the resolved system prompt, baked in here at
    // resolution time: config is static for the process lifetime, so it cannot
    // drift from what SkillTool will serve on invocation.
    const std::vector<std::string> resolved_skills =
        ResolveAllowedSkills(raw.allowed_skills, skills_);
    preset.system_prompt = raw.body + RenderSkillListing(resolved_skills, skills_);
    preset.provider.base_url = entry->base_url;
    preset.provider.api_key = entry->api_key;
    preset.provider.model_name = raw.model.empty() ? entry->model_name : raw.model;
    preset.allowed_tools = raw.allowed_tools;
    preset.allowed_skills = resolved_skills;
    preset.allowed_mcp_servers = raw.allowed_mcp_servers;
    preset.parameters = entry->parameters;
    preset.permission_mode = BuildPermissionPolicy(raw.permission_mode, permissions_.global_layer);
    preset.mcp_permissions = BuildMcpPermissionPolicy(raw.mcp_permissions, permissions_.mcp_default_mode);
    preset.run_mode = permissions_.default_run_mode;  // apply_run_mode() is applied once, downstream, by ToolExecutor
    preset.is_subagent = false;

    if (!raw.compression_prompt.empty()) {
        const std::filesystem::path prompt_path = config_dir_ / "prompts" / (raw.compression_prompt + ".md");
        preset.compression_prompt =
            config::parse_frontmatter_markdown(ReadFileRequired(prompt_path, "compression prompt")).body;
    } else {
        preset.compression_prompt = default_compression_prompt_;
    }

    return preset;
}

}  // namespace minimal::agent
