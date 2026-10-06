#include "minimal/agent/agent_preset_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
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
    common::ReadFileResult result = common::ReadFile(path);
    if (result.status == common::ReadFileResult::Status::kNotFound) {
        throw std::runtime_error(what + " not found: " + path.string());
    }
    if (!result.ok()) throw std::runtime_error(what + ": " + result.error);
    return std::move(result.contents);
}

}  // namespace

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SettingsConfig& settings,
                                         ToolPermissionKeys tool_permission_keys,
                                         const llm::VariantParamsRegistry* variant_params)
    : AgentPresetRegistry(config_dir, settings,
                          config::LoadPermissionsConfig(config_dir / "permissions.json"),
                          std::move(tool_permission_keys), variant_params) {}

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SettingsConfig& settings,
                                         const config::PermissionsConfig& permissions,
                                         ToolPermissionKeys tool_permission_keys,
                                         const llm::VariantParamsRegistry* variant_params)
    : AgentPresetRegistry(config_dir, settings, permissions,
                          std::make_unique<config::ProvidersConfig>(config::LoadProvidersConfig(
                              config_dir / "providers.json",
                              config::MakeVariantParamsHook(variant_params))),
                          nullptr, std::move(tool_permission_keys)) {}

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SettingsConfig& settings,
                                         const config::PermissionsConfig& permissions,
                                         const config::ProvidersConfig& providers,
                                         ToolPermissionKeys tool_permission_keys)
    : AgentPresetRegistry(config_dir, settings, permissions, nullptr, &providers,
                          std::move(tool_permission_keys)) {}

AgentPresetRegistry::AgentPresetRegistry(std::filesystem::path config_dir,
                                         const config::SettingsConfig& settings,
                                         const config::PermissionsConfig& permissions,
                                         std::unique_ptr<config::ProvidersConfig> owned,
                                         const config::ProvidersConfig* borrowed,
                                         ToolPermissionKeys tool_permission_keys)
    : config_dir_(std::move(config_dir)),
      owned_providers_(std::move(owned)),
      // owned_providers_ is declared (so initialized) first.
      providers_(borrowed != nullptr ? *borrowed : *owned_providers_),
      settings_(settings),
      permissions_(permissions),
      tool_permission_keys_(std::move(tool_permission_keys)) {
    // Both optional at this phase -- a real deployment's absence is the
    // startup sanity check's job to flag, not this constructor's.
    subagent_prefix_ = common::ReadFileOrEmpty(config_dir_ / "prompts" / "subagent.md");
    default_compression_prompt_ =
        config::parse_frontmatter_markdown(common::ReadFileOrEmpty(config_dir_ / "prompts" / "compression.md")).body;
}

AgentPreset AgentPresetRegistry::Resolve(
    const std::string& preset_name, const std::filesystem::path& requested_root,
    const AgentPreset* parent, const std::optional<config::ModelRef>& model_override) const {
    const std::filesystem::path project_root = EffectiveRoot(requested_root);
    if (parent != nullptr || model_override.has_value()) {
        // The clamp input varies per parent, so no cache: resolve fresh every
        // time (spawn is rare; a resolve is frontmatter reads plus a model
        // lookup, not a turn). A model_override is likewise per-caller state,
        // not a property of (preset_name, root) worth caching.
        return ResolveUncached(preset_name, project_root, parent, model_override);
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
    AgentPreset resolved = ResolveUncached(preset_name, canonical, nullptr, std::nullopt);

    std::lock_guard<std::mutex> lock(cache_mu_);
    return cache_.emplace(std::make_pair(preset_name, std::move(canonical)), std::move(resolved))
        .first->second;
}

std::vector<AgentPresetSummary> AgentPresetRegistry::ListPresets(
    const std::filesystem::path& project_root, const bool picker_visibility) const {
    // name -> summary, first (nearest) declaration wins: project presets are
    // inserted before the global scan and shadow a global preset of the same
    // name, matching Resolve()'s lookup order.
    std::map<std::string, AgentPresetSummary> merged;

    // Names the PROJECT declared, whatever their visibility. A project preset
    // that is disabled/hidden/subagent is still the preset Resolve() picks for
    // its name -- it just must not appear in a picker. Reserving the name here
    // (not merely skipping the insert) is what keeps the global scan from
    // listing a DIFFERENT preset under that name: the picker would offer a
    // global preset that Resolve() can never reach, because the project file
    // shadows it by name in every lookup.
    std::set<std::string> project_declared;

    // `record` collects every parseable id (project scans); `skip` suppresses
    // ids owned elsewhere (the global scan skips project-owned ids).
    //
    // Keyed on the FILE STEM, not raw.name (the frontmatter display value):
    // Resolve()/ResolveUncached() look presets up by filename, never by the
    // frontmatter `name:` field, which need not match it. Keying the listing
    // (and the shadowing set below) on raw.name meant a preset whose two
    // diverged was offered in the picker under a name Resolve() could never
    // find -- "unknown agent preset" on selection -- and, separately, that a
    // project preset's frontmatter name (rather than its filename) decided
    // whether it shadowed a global preset of the same FILENAME, which is not
    // the rule Resolve() itself applies. The stem is what both picker
    // selection and /agent <name> actually resolve against, so keying on it
    // is what makes every listed entry selectable.
    auto scan_dir = [&](const std::filesystem::path& agents_dir,
                        std::set<std::string>* record, const std::set<std::string>* skip) {
        std::error_code ec;
        if (!std::filesystem::exists(agents_dir, ec)) return;

        for (const auto& entry : std::filesystem::directory_iterator(agents_dir, ec)) {
            if (ec) break;
            if (!entry.is_regular_file() || entry.path().extension() != ".md") continue;
            const std::string id = entry.path().stem().string();
            try {
                const AgentPresetFrontmatter raw =
                    LoadAgentPresetFrontmatter(entry.path(), tool_permission_keys_.vocabulary());
                if (record != nullptr) record->insert(id);
                // Visibility rules. Picker: disabled presets cannot run at all;
                // `hidden` and `mode: subagent` presets are spawnable but not
                // switchable from the picker (spawn accepts them regardless).
                // Spawnable listing: only `disable` removes a preset -- mode
                // and hidden never gate spawnability.
                if (raw.disable) continue;
                if (picker_visibility && (raw.hidden || raw.mode == AgentMode::Subagent)) continue;
                if (skip != nullptr && skip->count(id) != 0) continue;
                merged.try_emplace(id, AgentPresetSummary{id, raw.description, raw.color});
            } catch (const std::exception& error) {
                // One malformed preset must not hide the rest from a listing.
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
            scan_dir(dir / "agents", &project_declared, nullptr);
        }
    }
    scan_dir(config_dir_ / "agents", nullptr, &project_declared);

    std::vector<AgentPresetSummary> out;
    out.reserve(merged.size());
    for (auto& [name, summary] : merged) out.push_back(std::move(summary));
    return out;  // map order == name order
}

std::optional<config::ModelRef> AgentPresetRegistry::DefaultModel() const {
    if (!settings_.default_model.empty()) {
        if (const auto ref = config::SplitModelRef(settings_.default_model);
            ref && providers_.Resolve(*ref) != nullptr) {
            return ref;
        }
    }
    // The only model: variants (<model>@<effort>) are siblings of one model.
    std::optional<std::string> only;
    for (const auto& ref_text : providers_.ModelRefs()) {
        const auto slash = ref_text.find('/');
        if (slash == std::string::npos) continue;
        const std::string base = ref_text.substr(0, slash + 1) +
                                 config::SplitVariantSuffix(ref_text.substr(slash + 1)).first;
        if (only && *only != base) return std::nullopt;  // several: the user picks
        only = base;
    }
    if (!only) return std::nullopt;
    return config::SplitModelRef(*only);
}

std::optional<std::string> AgentPresetRegistry::DefaultAgentPreset(
    const std::filesystem::path& project_root) const {
    if (!settings_.default_agent_preset.empty()) {
        try {
            bool from_project = false;
            const auto path = FindPresetPath(settings_.default_agent_preset,
                                             EffectiveRoot(project_root), from_project);
            if (!LoadAgentPresetFrontmatter(path, tool_permission_keys_.vocabulary()).disable) {
                return settings_.default_agent_preset;
            }
        } catch (const std::exception&) {
            // Missing or broken: the sanity check warned; fall through.
        }
    }
    const auto listed = ListAvailable(project_root);
    if (listed.size() == 1) return listed.front().name;
    return std::nullopt;
}

std::string AgentPresetRegistry::DescribeAgentPresets(
    const std::filesystem::path& project_root) const {
    std::string out;
    for (const auto& preset : ListAvailable(project_root)) {
        out += (out.empty() ? "" : ", ") + preset.name;
    }
    return out.empty() ? "none" : out;
}

std::vector<AgentPresetSummary> AgentPresetRegistry::ListAvailable(
    const std::filesystem::path& project_root) const {
    return ListPresets(EffectiveRoot(project_root), /*picker_visibility=*/true);
}

void AgentPresetRegistry::SetDefinitionBuilder(std::string tool_name,
                                               PresetToolDefinitionBuilder builder) {
    std::lock_guard<std::mutex> lock(cache_mu_);
    definition_builders_[std::move(tool_name)] = std::move(builder);
}

const PresetToolEnv& AgentPresetRegistry::EnvFor(const std::filesystem::path& project_root) const {
    // Same canonicalization the preset cache applies: symlinks must not
    // fragment the memo.
    std::filesystem::path canonical = project_root;
    if (!canonical.empty()) {
        std::error_code ec;
        auto resolved = std::filesystem::weakly_canonical(canonical, ec);
        if (!ec) canonical = std::move(resolved);
    }

    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        if (auto it = env_cache_.find(canonical); it != env_cache_.end()) return it->second;
    }

    // Computed OUTSIDE the lock, exactly like the preset cache: directory
    // scans behind the mutex would serialize concurrent resolves, and a race
    // is harmless -- both threads scan immutable files, and the insert below
    // keeps whichever landed first so every caller sees one consistent env.
    PresetToolEnv env;
    env.available_agents = ListPresets(project_root, /*picker_visibility=*/false);

    std::lock_guard<std::mutex> lock(cache_mu_);
    return env_cache_.emplace(std::move(canonical), std::move(env)).first->second;
}

void ApplyModel(AgentPreset& preset, const config::ResolvedModel& model) {
    preset.model_ref = model.ref;
    preset.provider_kind = model.backend;
    preset.provider = model.provider;
    preset.parameters = model.parameters;
    preset.max_tokens = model.max_tokens;
    preset.context_window_tokens = model.context_window_tokens;
}

std::filesystem::path AgentPresetRegistry::FindPresetPath(
    const std::string& preset_name, const std::filesystem::path& project_root,
    bool& from_project) const {
    if (preset_name.empty() || preset_name == "." || preset_name == ".." ||
        preset_name.find('/') != std::string::npos) {
        throw std::runtime_error("invalid agent preset name: '" + preset_name + "'");
    }

    // Project-local first, then the global config dir -- the nearest
    // declaration wins. An unknown name reports the GLOBAL path: it is the
    // one the operator configured, and the one the error message has always
    // named.
    const std::filesystem::path global_path = config_dir_ / "agents" / (preset_name + ".md");
    std::filesystem::path preset_path;
    from_project = false;
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
    return preset_path;
}

bool AgentPresetRegistry::PresetDeclaresModel(const std::string& preset_name,
                                              const std::filesystem::path& requested_root) const {
    bool from_project = false;
    const std::filesystem::path preset_path =
        FindPresetPath(preset_name, EffectiveRoot(requested_root), from_project);
    const std::string model =
        LoadAgentPresetFrontmatter(preset_path, tool_permission_keys_.vocabulary()).model;
    if (model.empty()) return false;
    // A declared model this machine's providers.json cannot serve (a project
    // preset written elsewhere) is a preference, not a requirement: warn and
    // let the spawn fall through to the next model in its chain.
    const auto ref = config::SplitModelRef(model);
    if (!ref || providers_.Resolve(*ref) == nullptr) {
        common::DefaultLogger().warn(
            "agent preset '{}' declares model '{}', which providers.json does not define; the "
            "subagent runs on its fallback model",
            preset_name, model);
        return false;
    }
    return true;
}

AgentPreset AgentPresetRegistry::ResolveUncached(
    const std::string& preset_name, const std::filesystem::path& project_root,
    const AgentPreset* parent,
    const std::optional<config::ModelRef>& model_override) const {
    // Shadowing is logged once per (name, root) because the cache makes
    // ResolveUncached run once per key -- FindPresetPath itself stays silent
    // so PresetDeclaresModel's peek doesn't double-log it.
    bool from_project = false;
    const std::filesystem::path preset_path = FindPresetPath(preset_name, project_root, from_project);
    const std::filesystem::path global_path = config_dir_ / "agents" / (preset_name + ".md");
    if (from_project && std::filesystem::exists(global_path)) {
        common::DefaultLogger().warn(
            "project agent preset '{}' ({}) shadows the global one", preset_name,
            preset_path.string());
    }
    const AgentPresetFrontmatter raw =
        LoadAgentPresetFrontmatter(preset_path, tool_permission_keys_.vocabulary());
    if (raw.disable) {
        // Refuse rather than resolve: a disabled preset must not run through
        // /agent, spawn, or a default_model reference.
        throw std::runtime_error("agent preset '" + preset_name + "' is disabled");
    }

    // The caller's own model choice outranks everything the preset or
    // settings.json says: it is a decision the caller already made (--model,
    // /model, a fork's pinned choice, a spawn's inherited override), and its
    // resolution failure must be reported in terms of THAT choice, not a
    // default_model the caller never touched. Without this, an unresolvable
    // settings.default_model throws below before a caller's own
    // ResolveModel-style override application ever runs -- the exact bug that
    // made a bad default_model brick session creation even when the caller
    // supplied a working --model. Resolved here (rather than left to the
    // caller, as ResolveModel-style callers also do) so a THROWING preset
    // fails before any of it is committed, and so every caller -- not just
    // the ones that remember to re-apply an override afterward -- gets the
    // rescue.
    std::shared_ptr<const config::ResolvedModel> resolved_model;
    if (model_override.has_value()) {
        resolved_model = providers_.Resolve(*model_override);
        if (!resolved_model) {
            throw std::runtime_error("unknown model '" + model_override->str() +
                                     "'; providers.json defines " +
                                     config::DescribeModels(providers_) +
                                     config::DescribeDiscovery(providers_, model_override->provider));
        }
    } else {
        // A preset's own `model:` applies only when it runs AS A SUBAGENT (the
        // spawn path, the only caller passing `parent`): a cheap explorer, a
        // fast reviewer. A primary session runs the user's choice, else
        // settings.default_model -- switching agents never switches models.
        const bool as_subagent = parent != nullptr;
        std::string model_text;
        if (as_subagent && !raw.model.empty()) {
            model_text = raw.model;
        } else if (const auto fallback = DefaultModel()) {
            model_text = fallback->str();
        } else {
            throw std::runtime_error(
                "no model chosen: providers.json defines " + config::DescribeModels(providers_) +
                " -- pick one (/model, --model) or set settings.json default_model");
        }
        const std::string source = model_text == raw.model && as_subagent
                                       ? "agent preset '" + preset_name + "'"
                                       : std::string("the default model");
        const auto ref = config::SplitModelRef(model_text);
        if (!ref) {
            throw std::runtime_error(source + ": model '" + model_text +
                                     "' must be <provider>/<model>, e.g. local/qwen4");
        }
        resolved_model = providers_.Resolve(*ref);  // snapshot; see ProvidersConfig::Resolve
        if (!resolved_model) {
            throw std::runtime_error(source + " names unknown model '" +
                                     model_text + "'; providers.json defines " +
                                     config::DescribeModels(providers_) +
                                     config::DescribeDiscovery(providers_, ref->provider));
        }
    }

    AgentPreset preset;
    preset.name = raw.name;
    preset.description = raw.description;
    preset.mode = raw.mode;
    preset.hidden = raw.hidden;
    preset.color = raw.color;
    preset.steps = raw.steps;
    // Plugin prompt appendices (preloaded skill bodies) join below, once the
    // preset's permission models are bound -- they read them.
    preset.system_prompt = raw.body;
    preset.model_declared = parent != nullptr && !raw.model.empty();
    ApplyModel(preset, *resolved_model);
    // Preset extras (temperature, top_p, reasoningEffort, ...) override the
    // model config's defaults: an agent declaring `temperature: 0.1` means it,
    // whatever providers.json says about the model.
    for (auto& [key, value] : raw.extra_parameters.items()) {
        preset.parameters[key] = value;
    }
    preset.allowed_tools = raw.allowed_tools;
    // `plugins:` groups expand HERE, into plain permission keys, so every
    // downstream consumer (the request's tool list, the subagent
    // intersection, the policy) sees one ordinary allowed list. A plugin that
    // is not loaded contributes nothing; the sanity check warns about it.
    for (const auto& plugin : raw.plugins) {
        const auto group = tool_permission_keys_.by_plugin.find(plugin);
        if (group == tool_permission_keys_.by_plugin.end()) continue;
        for (const auto& key : group->second) {
            if (std::find(preset.allowed_tools.begin(), preset.allowed_tools.end(), key) ==
                preset.allowed_tools.end()) {
                preset.allowed_tools.push_back(key);
            }
        }
    }
    // Spawn allowlist, resolved: "*" expands to no restriction (nullopt);
    // anything else carries verbatim -- an explicit EMPTY list stays empty,
    // meaning no spawns at all (the tool is withheld downstream and every
    // spawn request refused). Unlike allowed_skills, unknown entries are
    // warned about rather than dropped: absence-is-deny would make a typo
    // silently SHRINK the set, and naming it is the fail-loud direction.
    // The check itself runs below, where the spawnable listing is at hand.
    if (raw.allowed_subagents.has_value()) {
        if (std::find(raw.allowed_subagents->begin(), raw.allowed_subagents->end(), "*") !=
            raw.allowed_subagents->end()) {
            preset.allowed_subagents = std::nullopt;  // explicit "all"
        } else {
            preset.allowed_subagents = raw.allowed_subagents;
        }
    }
    preset.tool_sources = raw.tool_sources;
    // Per-tool plugin config rides verbatim; namespaced slices are delivered
    // per-invocation by the agent loop (ToolContext::tool_config).
    preset.tool_config = raw.tool_config;
    // A project-local preset (repo-shipped agents/*.md, discovered ahead of
    // config_dir per the walk above) is untrusted content: it may narrow the
    // operator's permissions.json floor but must never widen it. See the
    // preset_is_trusted doc on BuildPermissionPolicy.
    preset.permission_mode =
        BuildPermissionPolicy(raw.permission_mode, permissions_.global_layer, !from_project,
                              tool_permission_keys_.dotenv_protected);
    // Plugin permission models: each binds the operator's permissions.json
    // slice and this preset's own, under the same trust rule -- a model
    // enforces "a project preset may only narrow" for its own slice.
    if (permission_models_ != nullptr) {
        preset.permission_models = permission_models_->Bind(
            permissions_.plugin_slices, raw.plugin_permissions, !from_project);
    }
    preset.run_mode = permissions_.default_run_mode;  // apply_run_mode() is applied once, downstream, by ToolExecutor
    preset.is_subagent = false;

    if (!raw.compression_prompt.empty()) {
        preset.compression_prompt =
            LoadCompressionPromptFile(raw.compression_prompt, "agent preset '" + preset_name + "'");
        preset.custom_compression_prompt = true;
    } else {
        preset.compression_prompt = default_compression_prompt_;
    }
    // The NAME travels verbatim; resolving it against the strategy registry is
    // the loop's job at compaction time (the registry is still filling here --
    // plugins register after presets load -- and /agent re-resolution must
    // pick up strategies registered later). An unknown name is not a load
    // error: it warns once per session at first use and degrades to the
    // settings-level strategy, the same policy as an unresolved
    // memory.compaction_strategy.
    preset.compaction_strategy = raw.compaction_strategy;

    // --- Preset-scoped extensions: tool definitions and prompt appendices ----
    // Run ONCE here, on the frozen preset the cache is about to hold (and on
    // the uncached spawn path too, where an extension MUST re-run: `parent`
    // changes what a child may have). The builders' per-root inputs come
    // memoized from EnvFor, so N presets of one root scan agents/ once; a
    // deployment with neither a builder nor any allowlisted preset skips the
    // scan entirely.
    std::map<std::string, PresetToolDefinitionBuilder> builders;
    {
        std::lock_guard<std::mutex> lock(cache_mu_);
        builders = definition_builders_;
    }
    if (extensions_ != nullptr) {
        for (const auto& entry : extensions_->builders()) builders[entry.tool_name] = entry.build;
    }
    if (extensions_ != nullptr && !extensions_->appendices().empty()) {
        const PresetToolEnv no_env;
        const PresetToolContext ctx{preset, preset_name, no_env, project_root, parent};
        for (const auto& appendix : extensions_->appendices()) {
            preset.system_prompt += appendix.render(ctx);
        }
    }
    const bool need_listing =
        !builders.empty() ||
        (preset.allowed_subagents.has_value() && !preset.allowed_subagents->empty());
    if (need_listing) {
        const PresetToolEnv& env = EnvFor(project_root);
        // The typo warning promised above: an allowlist entry naming no
        // preset. The child could never resolve the name anyway ("unknown
        // agent preset" at spawn), but saying so at load points at the FILE.
        if (preset.allowed_subagents.has_value()) {
            for (const auto& entry : *preset.allowed_subagents) {
                const bool known = std::any_of(
                    env.available_agents.begin(), env.available_agents.end(),
                    [&entry](const AgentPresetSummary& summary) {
                        return summary.name == entry;
                    });
                if (!known) {
                    common::DefaultLogger().warn(
                        "agent preset '{}': allowed_subagents entry '{}' matches no agent preset",
                        preset_name, entry);
                }
            }
        }
        for (auto& [tool_name, builder] : builders) {
            preset.tool_definition_overrides[tool_name] =
                builder(PresetToolContext{preset, preset_name, env, project_root, parent});
        }
    }

    return preset;
}

std::string AgentPresetRegistry::LoadCompressionPromptFile(const std::string& name,
                                                           const std::string& error_context) const {
    // The name is used to build a path into prompts/, and presets (and command
    // clients) can be untrusted content: '/' or '\\' would climb into or
    // replace the prompts directory and read an arbitrary *.md into the
    // summarization prompt. A valid value is a bare file name; anything else
    // is the same kind of mistake as an invalid preset name.
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
        name == ".." || name == ".") {
        throw std::runtime_error(error_context + ": compression_prompt must be a file name, got '" +
                                 name + "'");
    }
    const std::filesystem::path prompt_path = config_dir_ / "prompts" / (name + ".md");
    return config::parse_frontmatter_markdown(ReadFileRequired(prompt_path, "compression prompt")).body;
}

std::string AgentPresetRegistry::ResolveCompressionPrompt(const std::string& name) const {
    return LoadCompressionPromptFile(name, "compression prompt '" + name + "'");
}

void PresetExtensionRegistry::AddDefinitionBuilder(std::string tool_name,
                                                   PresetToolDefinitionBuilder builder) {
    if (builder) builders_.push_back({attribution_, std::move(tool_name), std::move(builder)});
}

void PresetExtensionRegistry::AddPromptAppendix(PresetPromptAppendix appendix) {
    if (appendix) appendices_.push_back({attribution_, std::move(appendix)});
}

void PresetExtensionRegistry::RemoveByAttribution(const std::string& plugin_name) {
    const auto owned = [&](const auto& entry) { return entry.plugin == plugin_name; };
    builders_.erase(std::remove_if(builders_.begin(), builders_.end(), owned), builders_.end());
    appendices_.erase(std::remove_if(appendices_.begin(), appendices_.end(), owned),
                      appendices_.end());
}

std::vector<std::string> PresetExtensionRegistry::OwnedBy(const std::string& plugin_name) const {
    std::vector<std::string> out;
    for (const auto& entry : builders_) {
        if (entry.plugin == plugin_name) out.push_back(entry.tool_name + " definition");
    }
    for (const auto& entry : appendices_) {
        if (entry.plugin == plugin_name) out.push_back("system prompt appendix");
    }
    return out;
}

}  // namespace minimal::agent
