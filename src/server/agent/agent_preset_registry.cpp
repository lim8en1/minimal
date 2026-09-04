#include "minimal/agent/agent_preset_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "minimal/agent/agent_preset_frontmatter.hpp"
#include "minimal/agent/permission_resolution.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/frontmatter.hpp"
#include "minimal/config/project_dirs.hpp"

namespace minimal::agent {

namespace {

std::string ReadFileRequired(const std::filesystem::path& path, const std::string& what) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error(what + " not found: " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// Resolves the raw allowed_skills map against the session's merged skill view
// (global ∪ project overlay). Only LOADED skills appear: a failed skill's
// reason surfaces through SkillTool at invocation time, and unknown names are
// skipped here (the startup sanity check reports them at load time). `merged`
// is sorted, so both the resolved map and every rendering of it are
// deterministic -- they sit in the cached prefix and must not reorder turn to
// turn.
//
// allowed_skills ∩ loaded, with `*` expanded. An explicit entry wins over the
// "*" entry for the same name (specific beats catch-all, matching opencode's
// last-rule-wins spirit). Explicit deny entries resolve OUT -- absence is deny,
// which keeps the subagent clamp uniform -- so the resolved map holds only
// Preload/Allow/Ask. Computed ONCE and used for both the listing and
// AgentPreset::allowed_skills, so the prompt cannot advertise a skill the tool
// would refuse, nor the tool be offered with nothing listed.
std::map<std::string, SkillMode> ResolveAllowedSkills(
    const std::map<std::string, SkillMode>& allowed,
    const std::vector<config::SkillMeta>& merged) {
    const auto star = allowed.find("*");
    const bool has_star = star != allowed.end();
    const SkillMode star_mode =
        has_star && star->second != SkillMode::Deny ? star->second : SkillMode::Deny;

    std::map<std::string, SkillMode> resolved;
    for (const auto& meta : merged) {
        if (!meta.loaded) continue;
        const auto it = allowed.find(meta.name);
        const SkillMode mode = it != allowed.end() ? it->second : star_mode;
        if (mode == SkillMode::Deny) continue;
        resolved[meta.name] = mode;
    }
    return resolved;  // merged is sorted; the map keeps it so
}

const config::SkillMeta* FindInMerged(const std::vector<config::SkillMeta>& merged,
                                      const std::string& name) {
    for (const auto& meta : merged) {
        if (meta.name == name) return &meta;
    }
    return nullptr;
}

// The on-demand listing: Allow and Ask entries, one bullet each. Ask entries
// say so, so the model can prefer a no-friction skill when two cover the task.
std::string RenderSkillListing(const std::map<std::string, SkillMode>& resolved,
                               const std::vector<config::SkillMeta>& merged) {
    std::string out;
    for (const auto& [name, mode] : resolved) {
        if (mode == SkillMode::Preload) continue;  // not tool-callable
        const config::SkillMeta* meta = FindInMerged(merged, name);
        if (meta == nullptr) continue;  // unreachable: resolved came from merged
        if (out.empty()) {
            out = "\n## Available skills\n\nLoad a skill's full instructions with the `skill` tool.\n";
        }
        out += "- " + meta->name + ": " + meta->description +
               (mode == SkillMode::Ask ? " (asks before loading)" : "") + "\n";
    }
    return out;
}

// The preload half: full bodies under one header, plus the instruction that
// makes re-invocation pointless to the model. Returns the section (possibly
// empty) and DEMOTES an unreadable preload to on-demand (Allow) -- a skill that
// fails to open must fall back to being listed, never silently vanish.
std::string RenderPreloadedSkills(
    const std::map<std::string, SkillMode>& resolved,
    const config::SkillRegistry& skills, const std::filesystem::path& project_root,
    std::map<std::string, SkillMode>& tool_skills) {
    std::string out;
    for (const auto& [name, mode] : resolved) {
        if (mode != SkillMode::Preload) continue;
        const auto body = skills.LoadBodyFor(project_root, name);
        if (!body) {
            common::DefaultLogger().warn(
                "skill '{}' is marked preload but its body could not be read; "
                "falling back to on-demand loading",
                name);
            tool_skills[name] = SkillMode::Allow;
            continue;
        }
        // Successfully preloaded: OUT of the tool-side map. (A failed read
        // demotes to Allow above, so the name stays loadable on demand.)
        tool_skills.erase(name);
        if (out.empty()) {
            out = "\n## Preloaded skills\n\n"
                  "The full instructions for these skills are ALREADY in your context; "
                  "do not call the `skill` tool for them.\n\n";
        }
        out += "### " + name + "\n\n" + *body + "\n";
    }
    return out;
}

}  // namespace

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SkillRegistry& skills,
                                         const config::SettingsConfig& settings)
    : AgentPresetRegistry(config_dir, skills, settings,
                          config::LoadPermissionsConfig(config_dir / "permissions.json")) {}

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SkillRegistry& skills,
                                         const config::SettingsConfig& settings,
                                         const config::PermissionsConfig& permissions)
    : config_dir_(std::move(config_dir)),
      providers_(config::LoadProvidersConfig(config_dir_ / "providers.json")),
      settings_(settings),
      permissions_(permissions),
      skills_(skills) {
    // Both optional at this phase -- a real deployment's absence is the
    // startup sanity check's job to flag, not this constructor's.
    subagent_prefix_ = common::ReadFileOrEmpty(config_dir_ / "prompts" / "subagent.md");
    default_compression_prompt_ =
        config::parse_frontmatter_markdown(common::ReadFileOrEmpty(config_dir_ / "prompts" / "compression.md")).body;
}

AgentPreset AgentPresetRegistry::Resolve(
    const std::string& preset_name, const std::filesystem::path& project_root,
    const std::optional<std::map<std::string, SkillMode>>& parent_skills) const {
    if (parent_skills.has_value()) {
        // The clamp input varies per parent, so no cache: resolve fresh every
        // time (spawn is rare; a resolve is frontmatter reads plus a model
        // lookup, not a turn).
        return ResolveUncached(preset_name, project_root, parent_skills);
    }

    // The cache key canonicalizes the root: two sessions giving the same
    // directory through different symlinks must share one cache entry, not
    // resolve past each other forever.
    std::filesystem::path canonical = project_root;
    if (!canonical.empty()) {
        std::error_code ec;
        auto resolved = std::filesystem::weakly_canonical(canonical, ec);
        if (!ec) canonical = std::move(resolved);
    }

    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        if (auto it = cache_.find({preset_name, canonical}); it != cache_.end()) return it->second;
    }

    // Resolved OUTSIDE the lock: ResolveUncached reads files and can throw, and
    // holding the cache mutex across disk I/O would serialize every concurrent
    // session creation behind one preset parse. The cost is that two threads
    // racing the same unseen preset may both parse it -- harmless, since the
    // result is a pure function of files that do not change at runtime, and the
    // insert below keeps whichever landed first so every later caller still sees
    // one consistent value.
    AgentPreset resolved = ResolveUncached(preset_name, canonical, std::nullopt);

    std::lock_guard<std::mutex> lock(cache_mu_);
    return cache_.emplace(std::make_pair(preset_name, std::move(canonical)), std::move(resolved))
        .first->second;
}

std::vector<AgentPresetSummary> AgentPresetRegistry::ListAvailable(
    const std::filesystem::path& project_root) const {
    // name -> summary, first (nearest) declaration wins: project presets are
    // inserted before the global scan and shadow a global preset of the same
    // name, matching Resolve()'s lookup order.
    std::map<std::string, AgentPresetSummary> merged;

    auto scan_dir = [&merged](const std::filesystem::path& agents_dir) {
        std::error_code ec;
        if (!std::filesystem::exists(agents_dir, ec)) return;

        for (const auto& entry : std::filesystem::directory_iterator(agents_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
            try {
                const AgentPresetFrontmatter raw = LoadAgentPresetFrontmatter(entry.path());
                // Picker-visibility rules: disabled presets cannot run at all;
                // `hidden` and `mode: subagent` presets are spawnable but not
                // switchable from the picker (spawn accepts them regardless).
                if (raw.disable || raw.hidden || raw.mode == AgentMode::Subagent) continue;
                merged.try_emplace(raw.name,
                                   AgentPresetSummary{raw.name, raw.description, raw.color});
            } catch (const std::exception& error) {
                // One malformed preset must not hide the rest from a picker.
                common::DefaultLogger().warn("agent preset '{}' skipped in listing: {}",
                                             entry.path().string(), error.what());
            }
        }
    };

    if (!project_root.empty()) {
        // Named local for the same reason ResolveUncached names its walk: a
        // range-for over a temporary's member dangles.
        const config::ProjectDirs project_dirs(project_root);
        for (const auto& dir : project_dirs.dirs()) {
            scan_dir(dir / "agents");
        }
    }
    scan_dir(config_dir_ / "agents");

    std::vector<AgentPresetSummary> out;
    out.reserve(merged.size());
    for (auto& [name, summary] : merged) out.push_back(std::move(summary));
    return out;  // map order == name order
}

void ApplyModel(AgentPreset& preset, const config::ResolvedModel& model) {
    preset.model_ref = model.ref;
    preset.provider_kind = model.backend;
    preset.provider = model.provider;
    preset.parameters = model.parameters;
    preset.max_tokens = model.max_tokens;
    preset.context_window_tokens = model.context_window_tokens;
}

AgentPreset AgentPresetRegistry::ResolveUncached(
    const std::string& preset_name, const std::filesystem::path& project_root,
    const std::optional<std::map<std::string, SkillMode>>& parent_skills) const {
    if (preset_name.empty() || preset_name == "." || preset_name == ".." ||
        preset_name.find('/') != std::string::npos) {
        throw std::runtime_error("invalid agent preset name: '" + preset_name + "'");
    }

    // Project-local first, then the global config dir -- the nearest
    // declaration wins, and a project shadowing a global preset is logged once
    // per (name, root) because the cache makes ResolveUncached run once per
    // key. An unknown name reports the GLOBAL path: it is the one the operator
    // configured, and the one the error message has always named.
    const std::filesystem::path global_path = config_dir_ / "agents" / (preset_name + ".md");
    std::filesystem::path preset_path;
    bool from_project = false;
    if (!project_root.empty()) {
        // A NAMED local, not a range-for over a temporary's member: the walk
        // object must outlive the loop. `for (auto dir : Temp(...).dirs())`
        // binds the loop range to a reference INTO a temporary, whose lifetime
        // a returned-reference binding does not extend -- the loop would
        // iterate a destroyed vector.
        const config::ProjectDirs project_dirs(project_root);
        for (const auto& dir : project_dirs.dirs()) {
            const std::filesystem::path candidate = dir / "agents" / (preset_name + ".md");
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec) && !ec) {
                preset_path = candidate;
                from_project = true;
                break;
            }
        }
    }
    if (preset_path.empty()) preset_path = global_path;
    if (!std::filesystem::exists(preset_path)) {
        throw std::runtime_error("unknown agent preset: '" + preset_name + "'");
    }
    if (from_project && std::filesystem::exists(global_path)) {
        common::DefaultLogger().warn(
            "project agent preset '{}' ({}) shadows the global one", preset_name,
            preset_path.string());
    }
    const AgentPresetFrontmatter raw = LoadAgentPresetFrontmatter(preset_path);
    if (raw.disable) {
        // Refuse rather than resolve: a disabled preset must not run through
        // /agent, spawn, or a default_model reference.
        throw std::runtime_error("agent preset '" + preset_name + "' is disabled");
    }

    // The preset's model, or settings.default_model when it declares none. Both
    // are already known to be <provider>/<model>-shaped -- the frontmatter parser
    // checks its own, and an unparseable default_model is caught here rather than
    // being silently treated as absent.
    const std::string model_text = raw.model.empty() ? settings_.default_model : raw.model;
    if (model_text.empty()) {
        throw std::runtime_error("agent preset '" + preset_name +
                                 "' declares no model and settings.json has no default_model");
    }
    const auto ref = config::ParseModelRef(model_text);
    if (!ref) {
        throw std::runtime_error("agent preset '" + preset_name + "': model '" + model_text +
                                 "' must be <provider>/<model>, e.g. local/qwen4");
    }
    const auto model = providers_.Resolve(*ref);  // snapshot; see ProvidersConfig::Resolve
    if (!model) {
        throw std::runtime_error("agent preset '" + preset_name + "' names unknown model '" +
                                 model_text + "'; providers.json defines " +
                                 config::DescribeModels(providers_) +
                                 config::DescribeDiscovery(providers_, ref->provider));
    }

    AgentPreset preset;
    preset.name = raw.name;
    preset.description = raw.description;
    preset.mode = raw.mode;
    preset.hidden = raw.hidden;
    preset.color = raw.color;
    preset.steps = raw.steps;
    // The skill listing is part of the resolved system prompt, baked in here at
    // resolution time: config is static for the process lifetime, so it cannot
    // drift from what SkillTool will serve on invocation. The merged view is
    // taken for THIS session's project root, so a project's skills/ directory
    // is advertised in its own sessions and nobody else's. The parent ceiling
    // (spawn path) clamps BEFORE baking -- a demoted preload must never ship
    // its body -- and only then are preload entries consumed (their bodies
    // ship; they never reach the tool) while an unreadable body demotes to
    // on-demand rather than vanishing.
    const std::vector<config::SkillMeta> merged_skills = skills_.AllFor(project_root);
    std::map<std::string, SkillMode> resolved_skills =
        ResolveAllowedSkills(raw.allowed_skills, merged_skills);
    if (parent_skills.has_value()) {
        // Elementwise most-restrictive over the SkillMode lattice (Preload
        // widest .. Deny narrowest); a skill the parent lacks is deny.
        std::map<std::string, SkillMode> clamped;
        for (const auto& [skill, mode] : resolved_skills) {
            const auto it = parent_skills->find(skill);
            if (it == parent_skills->end()) continue;
            clamped[skill] = std::max(it->second, mode);
        }
        resolved_skills = std::move(clamped);
    }
    std::map<std::string, SkillMode> tool_skills = resolved_skills;
    const std::string preloaded = RenderPreloadedSkills(resolved_skills, skills_, project_root,
                                                        tool_skills);
    preset.system_prompt = raw.body + RenderSkillListing(tool_skills, merged_skills) + preloaded;
    ApplyModel(preset, *model);
    // Preset extras (temperature, top_p, reasoningEffort, ...) override the
    // model config's defaults: an agent declaring `temperature: 0.1` means it,
    // whatever providers.json says about the model.
    for (auto& [key, value] : raw.extra_parameters.items()) {
        preset.parameters[key] = value;
    }
    preset.allowed_tools = raw.allowed_tools;
    preset.allowed_skills = tool_skills;
    preset.allowed_mcp_servers = raw.allowed_mcp_servers;
    // A project-local preset (repo-shipped agents/*.md, discovered ahead of
    // config_dir per the walk above) is untrusted content: it may narrow the
    // operator's permissions.json floor but must never widen it. See the
    // preset_is_trusted doc on BuildPermissionPolicy.
    preset.permission_mode =
        BuildPermissionPolicy(raw.permission_mode, permissions_.global_layer, !from_project);
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
