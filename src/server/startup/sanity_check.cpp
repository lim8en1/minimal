#include "minimal/startup/sanity_check.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/common/config_paths.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/metadata_catalog.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/llm/variant_params.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/tools/pattern_match.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/permission_model.hpp"
#include "minimal/tools/tool_source.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::startup {

using namespace minimal::config;
namespace {

CheckResult Fatal(const std::string& check, const std::string& message) {
    return {CheckSeverity::Fatal, check, message};
}

CheckResult Warning(const std::string& check, const std::string& message) {
    return {CheckSeverity::Warning, check, message};
}

std::string JoinNames(const std::vector<std::string>& names) {
    if (names.empty()) return "nothing";
    if (names.size() == 1) return names.front();
    std::string out = names.front();
    for (std::size_t i = 1; i + 1 < names.size(); ++i) out += ", " + names[i];
    return out + " and " + names.back();
}

}  // namespace

StartupSanityCheck::StartupSanityCheck(const ConfigBundle& bundle, SanityCheckInputs inputs)
    : bundle_(bundle), inputs_(std::move(inputs)) {
    if (inputs_.registries.tools == nullptr || inputs_.registries.providers == nullptr) {
        throw std::invalid_argument("StartupSanityCheck needs the tool and provider registries");
    }
}

std::vector<CheckResult> StartupSanityCheck::RunAll() {
    std::vector<CheckResult> results;
    // Order matters only for readability: EVERY check runs regardless of what
    // the earlier ones found -- that is the linter property. The checks read
    // whatever the bundle managed to load.
    auto append = [&results](std::vector<CheckResult> more) {
        results.insert(results.end(), std::make_move_iterator(more.begin()),
                       std::make_move_iterator(more.end()));
    };
    append(check_providers());
    append(check_presets());
    append(check_plugin_warnings());
    append(check_directories());
    append(check_plugins());
    append(check_name_collisions());
    append(check_naming_model());
    append(check_hooks());
    append(check_advisor());
    append(check_subagent_limit());
    append(check_compaction_strategy());
    append(check_compaction_keep_recent());
    append(check_small_model());
    append(check_default_subagent_model());
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_providers() {
    std::vector<CheckResult> results;
    if (!bundle_.providers_loaded) return results;  // already Fatal under config_parse

    // A provider is a CONNECTION and the addressable unit is <provider>/<model>,
    // so this walks models rather than entries: a provider whose backend no
    // plugin registered makes every model under it unusable, and saying so once
    // per model is what tells the user which references are dead.
    const auto kinds = inputs_.registries.providers->kinds();
    const auto refs = bundle_.providers.ModelRefs();
    bool any_usable = false;
    std::unordered_map<std::string, bool> backend_reported;

    for (const auto& ref_text : refs) {
        const auto ref = common::SplitModelRef(ref_text);
        if (!ref) continue;  // truly malformed; ParseProvidersConfig rejects those
        const auto model = bundle_.providers.Resolve(*ref);
        if (model == nullptr) continue;

        const bool registered =
            std::find(kinds.begin(), kinds.end(), model->backend) != kinds.end();
        if (!registered) {
            // Once per provider, not once per model: five models behind one dead
            // backend is one problem, and five copies of it buries the others.
            if (!backend_reported[ref->provider]) {
                backend_reported[ref->provider] = true;
                results.push_back(Warning("usable_provider",
                                          "providers.json provider '" + ref->provider +
                                              "': backend '" + model->backend +
                                              "' is not registered by any loaded plugin"));
            }
            continue;
        }
        any_usable = true;
    }

    // Discover-entries (track N): addressable through their backend's catalog
    // even with no static model. Checked STRUCTURALLY -- which plugins
    // registered what -- never by probing, because discovery is fetched lazily
    // at first resolve, never at startup. A discover-entry with a registered
    // backend counts as usable: whether the catalog actually answers is a
    // runtime condition, reported at first resolve and by ListModels'
    // discovery refresh (the /model picker's ctrl+r).
    for (const auto& [name, entry] : bundle_.providers.provider_entries()) {
        if (!entry.discover) continue;
        const bool backend_registered =
            std::find(kinds.begin(), kinds.end(), entry.backend) != kinds.end();
        if (!backend_registered) {
            if (!backend_reported[name]) {
                results.push_back(Warning(
                    "usable_provider", "providers.json provider '" + name + "': backend '" +
                                           entry.backend +
                                           "' is not registered by any loaded plugin; discovery "
                                           "cannot run and its static models do not resolve"));
            }
            continue;
        }
        any_usable = true;
        const bool catalog_registered =
            inputs_.registries.model_catalogs != nullptr && inputs_.registries.model_catalogs->Has(entry.backend);
        if (!catalog_registered) {
            results.push_back(
                Warning("usable_provider",
                        "providers.json provider '" + name + "': sets \"discover\": true but no "
                        "loaded plugin registers a model catalog for backend '" +
                            entry.backend +
                            "'; discovery will fail at first resolve (its static models, if any, "
                            "still resolve)"));
        }
    }
    if (!any_usable) {
        // A harness that cannot talk to an LLM is useless, not degraded -- the
        // same standard as a failed bundled plugin.
        const std::string available = kinds.empty() ? "none" : JoinNames(kinds);
        results.push_back(Fatal(
            "usable_provider",
            refs.empty()
                ? "no usable model: providers.json defines none at all -- add a provider with a "
                  "nested \"models\" object, and a backend a loaded plugin registers (available "
                  "kinds: " +
                      available + "). See providers.example.json."
                : "no usable model: none of the configured providers uses a registered backend "
                  "(available kinds: " +
                      available + ")"));
    }

    // The default has to name something, or every session that does not override
    // it fails at creation with the same message.
    if (bundle_.settings_loaded && !bundle_.settings.default_model.empty()) {
        // SplitModelRef: default_model is the same user-facing surface as
        // --model/`/model` and an agent preset's `model:`, and must accept
        // whatever a discovered catalog's slash-bearing model id those do.
        const auto ref = common::SplitModelRef(bundle_.settings.default_model);
        if (!ref) {
            results.push_back(Warning("usable_provider",
                                      "settings.json default_model '" + bundle_.settings.default_model +
                                          "' is not <provider>/<model>"));
        } else if (bundle_.providers.Resolve(*ref) == nullptr) {
            // Under a discover-entry a non-static model cannot be VERIFIED at
            // startup without a network call, which the check never makes -- the
            // provider exists and discovery answers at first resolve. That is
            // not a reason to stay silent, though: a typo'd or stale discovered
            // model id is exactly as likely as a static one, and the operator
            // deserves the same heads-up at startup rather than finding out only
            // at first session creation. So this still warns under discovery,
            // worded to say "unverified", not "matches no configured model".
            const auto entry = bundle_.providers.provider_entries().find(ref->provider);
            const bool unverifiable = entry != bundle_.providers.provider_entries().end() &&
                                      entry->second.discover;
            if (unverifiable) {
                results.push_back(Warning(
                    "usable_provider",
                    "settings.json default_model '" + bundle_.settings.default_model +
                        "' is not in provider '" + ref->provider +
                        "'s static model list; it may only resolve through discovery, which this "
                        "check cannot verify without a network call -- confirm it after startup"));
            } else {
                results.push_back(Warning("usable_provider",
                                          "settings.json default_model '" + bundle_.settings.default_model +
                                              "' matches no configured model" +
                                              (refs.empty() ? "" : " (have: " + JoinNames(refs) + ")")));
            }
        }
    }

    // A kind two plugins both register: the first wins silently -- make it loud.
    // The registries' own collision record is reported as Fatal by
    // check_name_collisions; a Warning for the same kind would say "the first
    // wins" right beside an error saying the server cannot start.
    const auto fatal_elsewhere = [](const auto* registry, const std::string& kind) {
        if (registry == nullptr) return false;
        for (const auto& collision : registry->collisions()) {
            if (collision.kind == kind) return true;
        }
        return false;
    };
    std::unordered_map<std::string, std::string> kind_owner;
    for (const auto& record : inputs_.plugin_records) {
        if (record.status != plugins::LoadStatus::Loaded) continue;
        const auto label = record.name.empty() ? record.file.filename().string() : record.name;
        for (const auto& kind : record.Registered("provider")) {
            const auto [it, inserted] = kind_owner.try_emplace(kind, label);
            if (!inserted && !fatal_elsewhere(inputs_.registries.providers, kind)) {
                results.push_back(Warning("provider_kinds", "provider kind '" + kind + "' is registered by both " +
                                     it->second + " and " + label + "; the first wins"));
            }
        }
    }

    // Same for catalogs (track N): they share the provider kind string, but are
    // a separate namespace -- a duplicate catalog silently determines what
    // "discover": true lists.
    std::unordered_map<std::string, std::string> catalog_owner;
    for (const auto& record : inputs_.plugin_records) {
        if (record.status != plugins::LoadStatus::Loaded) continue;
        const auto label = record.name.empty() ? record.file.filename().string() : record.name;
        for (const auto& kind : record.Registered("catalog")) {
            const auto [it, inserted] = catalog_owner.try_emplace(kind, label);
            if (!inserted && !fatal_elsewhere(inputs_.registries.model_catalogs, kind)) {
                results.push_back(Warning("provider_catalogs",
                                          "model catalog for kind '" + kind +
                                              "' is registered by both " + it->second + " and " +
                                              label + "; the first wins"));
            }
        }
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_presets() {
    std::vector<CheckResult> results;
    const auto agents_dir = inputs_.config_dir / "agents";

    // Parse every preset EAGERLY: AgentPresetRegistry loads them lazily, which
    // is exactly why a broken one stayed silent until the first session.
    std::error_code ec;
    if (std::filesystem::is_directory(agents_dir, ec)) {
        // increment(ec), not the range-for. `directory_iterator(dir, ec)` only makes
    // CONSTRUCTION non-throwing; the range-for's operator++ is the throwing
    // overload, so one unreadable entry threw straight out of RunAll and skipped
    // every later check -- breaking the linter property this class documents
    // ("EVERY check runs regardless of what the earlier ones found").
    // skill_registry.cpp:153 already had the right shape.
    for (auto it = std::filesystem::directory_iterator(agents_dir, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".md") continue;
            const auto preset_name = entry.path().stem().string();
            try {
                agent::ToolVocabulary vocabulary(inputs_.registries.tools->PermissionKeys());
                vocabulary.config_aliases = inputs_.registries.tools->ConfigAliasPermissionKeys();
                vocabulary.no_pattern_rules = inputs_.registries.tools->NoPatternRulePermissionKeys();
                presets_[preset_name] = agent::LoadAgentPresetFrontmatter(entry.path(), vocabulary);
                for (const auto& key : presets_[preset_name].unknown_permission_keys) {
                    results.push_back(Warning("presets", "agents/" + preset_name +
                                                             ".md: unknown permission_mode key '" +
                                                             key + "' is ignored (typo?)"));
                }
            } catch (const std::exception& e) {
                results.push_back(Warning("presets", "agents/" + preset_name + ".md: " + e.what()));
            }
        }
    }

    if (presets_.empty()) {
        // No session can be created at all -- the cold-start case. Loud by
        // design: there is no onboarding wizard, just a check with a reason.
        results.push_back(Fatal("presets", "no agent preset available -- create agents/<name>.md "
                             "(frontmatter: name, model: <provider>/<model>; body: system prompt) and set "
                             "settings.json default_agent_preset"));
    }

    // Resolves the preset's model the way AgentPresetRegistry would: the
    // preset's own <provider>/<model>, or settings.default_model when it
    // declares none.
    const auto preset_problem = [this](const agent::AgentPresetFrontmatter& raw) -> std::optional<std::string> {
        if (!bundle_.providers_loaded) return std::nullopt;  // config_parse already names it
        std::string model_text = raw.model;
        if (model_text.empty()) {
            model_text = bundle_.settings_loaded ? bundle_.settings.default_model : "";
            if (model_text.empty()) {
                return "declares no model and settings.json sets no default_model";
            }
        }
        // SplitModelRef, matching AgentPresetRegistry::Resolve and
        // ParseAgentPresetFrontmatter's own shape check (see their comments).
        const auto ref = common::SplitModelRef(model_text);
        if (!ref) return "model '" + model_text + "' is not <provider>/<model>";
        // A discover-entry's backend kind can be checked without the model
        // resolving -- the model list is a runtime answer the check never
        // probes.
        std::string discover_backend;
        if (auto entry = bundle_.providers.provider_entries().find(ref->provider);
            entry != bundle_.providers.provider_entries().end() && entry->second.discover) {
            discover_backend = entry->second.backend;
        }
        const auto model = bundle_.providers.Resolve(*ref);
        if (model == nullptr && discover_backend.empty()) {
            return "names unknown model '" + model_text + "'";
        }
        const auto kinds = inputs_.registries.providers->kinds();
        const std::string& backend = model != nullptr ? model->backend : discover_backend;
        if (std::find(kinds.begin(), kinds.end(), backend) == kinds.end()) {
            return "model '" + model_text + "' needs backend '" + backend +
                   "', which no loaded plugin registers";
        }
        return std::nullopt;
    };

    // The DEFAULT preset must resolve end to end: every client that does not
    // name a preset (the headless client's common case) would fail otherwise.
    if (bundle_.settings_loaded && !bundle_.settings.default_agent_preset.empty()) {
        const auto it = presets_.find(bundle_.settings.default_agent_preset);
        if (it == presets_.end()) {
            results.push_back(Fatal("presets", "default_agent_preset '" + bundle_.settings.default_agent_preset +
                                 "' does not resolve: agents/" + bundle_.settings.default_agent_preset +
                                 ".md is missing or failed to parse"));
        } else if (auto problem = preset_problem(it->second)) {
            results.push_back(Fatal("presets", "default_agent_preset '" + bundle_.settings.default_agent_preset +
                                 "' does not resolve: " + *problem));
        }
    }

    for (const auto& [preset_name, raw] : presets_) {
        const bool is_default = bundle_.settings_loaded && preset_name == bundle_.settings.default_agent_preset;
        // Non-default presets break only the sessions that pick them; a broken
        // provider chain on the DEFAULT preset was just reported as Fatal above,
        // and repeating it as a Warning would read as two problems instead of
        // one. The compression-prompt check still applies to it: a default
        // preset's missing custom prompt is a distinct problem.
        if (!is_default) {
            if (auto problem = preset_problem(raw)) {
                results.push_back(Warning("presets", "agent preset '" + preset_name + "': " + *problem));
            }
        }
        if (!raw.compression_prompt.empty() &&
            !std::filesystem::exists(inputs_.config_dir / "prompts" / (raw.compression_prompt + ".md"), ec)) {
            results.push_back(Warning("presets", "agent preset '" + preset_name +
                                 "': compression prompt 'prompts/" + raw.compression_prompt + ".md' not found"));
        }
    }

    // The default compaction prompt: absence degrades every session's compaction
    // to a log line, and making that loud is this check's job (ARCHITECTURE.md,
    // "Bundled assets").
    if (!std::filesystem::exists(inputs_.config_dir / "prompts" / "compression.md", ec)) {
        results.push_back(Warning("presets", "prompts/compression.md not found -- compaction is disabled "
                             "for every preset that does not override it"));
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_plugin_warnings() {
    // Problems a plugin found in its own configuration (report_warning):
    // printed beside core's findings, under the plugin's name, so a plugin
    // that owns a config file is as loud about it as core is about its own.
    std::vector<CheckResult> results;
    for (const auto& record : inputs_.plugin_records) {
        if (record.status != plugins::LoadStatus::Loaded) continue;
        for (const auto& warning : record.warnings) {
            results.push_back(Warning(record.name, warning));
        }
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_directories() {
    std::vector<CheckResult> results;
    auto check_dir = [](const std::filesystem::path& path, const char* what) -> std::optional<std::string> {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec) || ec) {
            return std::string(what) + " '" + path.string() + "' does not exist or is not a directory";
        }
        if (::faccessat(AT_FDCWD, path.c_str(), W_OK, AT_EACCESS) != 0) {
            return std::string(what) + " '" + path.string() + "' is not writable";
        }
        return std::nullopt;
    };

    // The server's own footprint. project_root itself is per-session (the client
    // supplies its cwd in CreateSessionRequest), so it is validated there, not
    // here -- see SessionServiceImpl::CreateSession.
    if (auto problem = check_dir(inputs_.config_dir, "config dir")) {
        results.push_back(Fatal("directories", *problem));
    }
    if (auto problem = check_dir(common::DataDir(), "data dir")) {
        results.push_back(Fatal("directories", *problem + " -- the session database and logs live there"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_plugins() {
    std::vector<CheckResult> results;
    // Two .so files declaring the same minimal_plugin_name are one plugin
    // twice as far as everything downstream is concerned: the data dir under
    // plugin_data_root, the storage namespace, and every attribution key are
    // all keyed by the declared name, so the second load silently shares (and
    // corrupts) the first's state, and collision diagnostics blame one plugin
    // for both. Name resolution cannot see this -- it receives already-loaded
    // attributions -- so it is checked here, against the records, naming both
    // files the way the provider-kind fatal below names both claimants.
    std::unordered_map<std::string, std::string> name_owner;
    for (const auto& record : inputs_.plugin_records) {
        if (record.status != plugins::LoadStatus::Loaded || record.name.empty()) continue;
        const auto [it, inserted] = name_owner.try_emplace(record.name, record.file.string());
        if (!inserted) {
            results.push_back(Fatal(
                "plugins", "plugin name '" + record.name + "' is declared by both " + it->second +
                               " and " + record.file.string() +
                               " -- they would share one data dir and storage namespace; "
                               "rename one of them (minimal_plugin_name)"));
        }
    }
    for (const auto& record : inputs_.plugin_records) {
        // Loaded is fine. Disabled is a DELIBERATE choice -- the documented
        // tool-replacement flow -- and its practical impact (no provider kind,
        // no preset tools) is caught by usable_provider/presets instead.
        if (record.status == plugins::LoadStatus::Loaded || record.status == plugins::LoadStatus::Disabled) {
            continue;
        }
        const auto label = record.name.empty() ? record.file.filename().string() : record.name;
        const std::string provenance =
            record.provenance == plugins::Provenance::Bundled ? "bundled" : "user";
        const std::string message = "plugin '" + label + "' (" + provenance + "): " +
                                    std::string(plugins::ToString(record.status)) +
                                    (record.error.empty() ? "" : ": " + record.error);
        // Severity splits by provenance: losing tools-fs or provider-openai is a
        // useless harness, not a degraded one; a user plugin is swappable.
        if (record.provenance == plugins::Provenance::Bundled) {
            results.push_back(Fatal("plugins", message));
        } else {
            results.push_back(Warning("plugins", message));
        }
    }
    return results;
}

namespace {
// An empty attribution means core registered it itself, which reads badly as ''.
std::string PluginLabel(const std::string& plugin_name) {
    return plugin_name.empty() ? std::string("the harness itself") : "plugin '" + plugin_name + "'";
}
}  // namespace

std::vector<CheckResult> StartupSanityCheck::check_name_collisions() {
    std::vector<CheckResult> results;
    const auto& resolution = inputs_.name_resolution;

    for (const auto& collision : resolution.collisions) {
        std::vector<std::string> owners;
        for (const auto& plugin_name : collision.claimants) {
            owners.push_back(plugin_name.empty() ? "core" : plugin_name);
        }
        const std::string by = JoinNames(owners);
        std::string message;
        switch (collision.space) {
            case plugins::NameSpace::kTool:
                message = "tool name '" + collision.surface + "' is provided by " + by +
                          " -- disable one plugin, or give one of them an aliases entry";
                break;
            case plugins::NameSpace::kCommand:
                message = "command name '/" + collision.surface + "' is provided by " + by +
                          " -- disable one plugin, or give one of them an aliases entry";
                break;
            case plugins::NameSpace::kService:
                message = "service name '" + collision.surface + "' is provided by " + by +
                          " -- disable one of the plugins";
                break;
            case plugins::NameSpace::kHookPoint:
                message = "hook point '" + collision.surface + "' is registered by " + by +
                          " -- disable one of the plugins";
                break;
        }
        results.push_back(Fatal("name_collisions", message));
    }

    for (const auto& key : resolution.dangling_aliases) {
        results.push_back(Warning("aliases", "aliases key '" + key +
                                  "' resolves to nothing registered (typo, or the providing plugin is disabled)"));
    }
    for (const auto& key : resolution.slot_aliases) {
        results.push_back(Warning("aliases", "aliases key '" + key +
                                  "' names a service or hook point, which cannot be aliased "
                                  "(aliases rename tools and commands) -- ignored"));
    }

    // Dotted plugin names that actually COMPOSE ambiguously: ("a.b","c") and
    // ("a","b.c") both form the stable identity "a.b.c", so no alias or rule
    // can address one claimant without governing both. Fatal like a surface
    // collision -- the Register-time dot warning alone left the resolution
    // order-dependent. Renaming one of the plugin files (or aliasing within
    // one plugin) is the fix.
    for (const auto& identity : resolution.ambiguous_identities) {
        results.push_back(Fatal(
            "name_collisions",
            "stable identity '" + identity +
                "' is composed ambiguously by two or more registered names (a plugin name "
                "containing a dot collides across ('a.b','c') and ('a','b.c')) -- rename one of "
                "the plugins"));
    }

    // The kind-keyed registries (AttributedRegistry): first registration wins
    // and the loser is recorded. Fatal, like tool and command collisions --
    // otherwise which implementation serves a kind depends on plugin load
    // order, and there is no alias mechanism for a kind, so disabling one
    // plugin is the only fix.
    const auto report = [&](const char* what, const auto& collisions) {
        for (const auto& collision : collisions) {
            results.push_back(Fatal("name_collisions",
                                    std::string(what) + " '" + collision.kind +
                                        "' is registered by both " +
                                        PluginLabel(collision.first_plugin) + " and " +
                                        PluginLabel(collision.second_plugin) +
                                        " -- disable one of them; kinds cannot be aliased"));
        }
    };
    report("provider kind", inputs_.registries.providers->collisions());
    if (inputs_.registries.model_catalogs != nullptr) {
        report("model catalog kind", inputs_.registries.model_catalogs->collisions());
    }
    if (inputs_.registries.metadata_catalogs != nullptr) {
        report("metadata catalog kind", inputs_.registries.metadata_catalogs->collisions());
    }
    if (inputs_.registries.variant_params != nullptr) {
        report("variant params kind", inputs_.registries.variant_params->collisions());
    }
    if (inputs_.registries.compaction_strategies != nullptr) {
        report("compaction strategy", inputs_.registries.compaction_strategies->collisions());
    }
    if (inputs_.registries.tool_sources != nullptr) {
        report("tool source", inputs_.registries.tool_sources->collisions());
    }
    // Permission models: one per plugin (a second from the same plugin is a
    // collision), and a model may only claim its own plugin's tools --
    // ruling on another plugin's tools is exactly the power the seam
    // withholds (tools/permission_model.hpp).
    if (inputs_.registries.permission_models != nullptr) {
        report("permission model", inputs_.registries.permission_models->collisions());
        for (const auto& error :
             inputs_.registries.permission_models->ValidateClaims(*inputs_.registries.tools)) {
            results.push_back(Fatal("name_collisions", error));
        }
    }

    // Per-preset tool_config namespaces: the same fatal rule, one level up.
    // Two plugins claiming one namespace would make which slice a tool read
    // depend on plugin load order -- the exact outcome collision detection
    // exists to prevent. There is no alias mechanism for a namespace.
    const auto namespace_report = inputs_.registries.tools->ConfigNamespaces();
    for (const auto& collision : namespace_report.collisions) {
        results.push_back(
            Fatal("name_collisions",
                  "tool_config namespace '" + collision.ns + "' is claimed by both " +
                      PluginLabel(collision.first_plugin) + " and " +
                      PluginLabel(collision.second_plugin) +
                      " -- disable one of them; config namespaces cannot be aliased"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_naming_model() {
    std::vector<CheckResult> results;
    const auto& resolution = inputs_.name_resolution;

    // The valid permission keys, and which bare names are ambiguous (two
    // plugins provide the original name -- the same count name resolution uses
    // to decide when a key must be qualified).
    std::unordered_set<std::string> valid_keys;
    for (const auto& [surface, key] : resolution.permission_keys) valid_keys.insert(key);
    std::unordered_map<std::string, int> original_counts;
    for (const auto& attr : inputs_.registries.tools->attributions()) ++original_counts[attr.tool_name];
    const auto ambiguous = [&original_counts](const std::string& name) { return original_counts[name] > 1; };

    // per_tool_overrides keys (global and per-preset) must reference a permission key.
    const auto* tools = inputs_.registries.tools;
    const auto check_override_key = [&results, &valid_keys, &ambiguous, tools](
                                        const std::string& where, const std::string& key) {
        // A key inside a namespace a plugin reserved names a tool that plugin
        // registers at runtime (after startup) -- nothing to check it against.
        if (tools->IsReserved(key)) return;
        if (ambiguous(key)) {
            results.push_back(Warning("naming_model", where + " per_tool_overrides key '" + key +
                                  "' is ambiguous -- two plugins provide that name; write it qualified as "
                                  "'<plugin>." + key + "'"));
        } else if (!valid_keys.count(key)) {
            results.push_back(Warning("naming_model", where + " per_tool_overrides key '" + key +
                                  "' resolves to no registered tool (typo, or the providing plugin is disabled)"));
        }
    };
    for (const auto& [key, mode] : bundle_.permissions.global_layer.per_tool_overrides) {
        check_override_key("permissions.json", key);
    }
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& [key, mode] : raw.permission_mode.per_tool_overrides) {
            check_override_key("agent preset '" + preset_name + "'", key);
        }
    }

    // Pattern rule keys reference permission keys exactly like per_tool_overrides
    // keys do (a pattern is operator intent on one tool, not a category), and the
    // globs are worth a budget warning: a huge rule list is quadratic-ish at
    // resolve time and almost always a mis-paste. Pattern KEYS are never globs
    // themselves -- a "git *" KEY is a tool-name typo, not a rule.
    const auto check_pattern_rules = [&results, &valid_keys](
                                         const std::string& where, const std::string& key,
                                         const std::vector<tools::PermissionPatternRule>& rules) {
        if (!valid_keys.count(key)) {
            results.push_back(Warning("naming_model", where + " pattern_overrides key '" + key +
                                  "' resolves to no registered tool (typo, or the providing plugin is disabled)"));
        }
        if (rules.size() > 200) {
            results.push_back(Warning("naming_model", where + " pattern_overrides for '" + key +
                                  "' carries " + std::to_string(rules.size()) +
                                  " rules; resolve cost grows with the list"));
        }
    };
    for (const auto& [key, rules] : bundle_.permissions.global_layer.pattern_overrides) {
        check_pattern_rules("permissions.json", key, rules);
    }
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& [key, rules] : raw.permission_mode.pattern_overrides) {
            check_pattern_rules("agent preset '" + preset_name + "'", key, rules);
        }
    }
    if (bundle_.permissions.global_layer.out_of_workdir_patterns.size() > 200) {
        results.push_back(Warning("naming_model",
                                  "permissions.json external_directory_patterns carries " +
                                      std::to_string(bundle_.permissions.global_layer.out_of_workdir_patterns.size()) +
                                      " rules; resolve cost grows with the list"));
    }

    // allowed_tools entries reference permission keys too -- never aliases.
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& tool : raw.allowed_tools) {
            if (tool == "*" || valid_keys.count(tool)) continue;
            results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                  "': allowed_tools entry '" + tool + "' matches no registered tool"));
        }
    }

    // tool_sources members must name a member the source knows; the source
    // must be registered by a loaded plugin.
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& [source_name, members] : raw.tool_sources) {
            const auto source = inputs_.registries.tool_sources != nullptr
                                    ? inputs_.registries.tool_sources->Find(source_name)
                                    : nullptr;
            if (source == nullptr) {
                results.push_back(Warning("naming_model",
                                          "agent preset '" + preset_name + "': tool source '" +
                                              source_name +
                                              "' is provided by no loaded plugin"));
                continue;
            }
            const auto known = source->Members();
            for (const auto& member : members) {
                if (std::find(known.begin(), known.end(), member) == known.end()) {
                    results.push_back(Warning("naming_model",
                                              "agent preset '" + preset_name + "': tool source '" +
                                                  source_name + "' has no member '" + member +
                                                  "'"));
                }
            }
        }
    }

    const auto* models = inputs_.registries.permission_models;
    const auto has_model = [models](const std::string& name) {
        return models != nullptr && models->Has(name);
    };
    // Each model lints its own slices (names that match nothing): the
    // operator's global slice once, then every preset's.
    if (models != nullptr) {
        const auto& globals = bundle_.permissions.plugin_slices;
        for (const auto& name : models->kinds()) {
            const auto* model = models->FindEntry(name);
            if (model == nullptr || *model == nullptr) continue;
            const nlohmann::json global =
                globals.is_object() && globals.contains(name) ? globals.at(name) : nlohmann::json();
            const auto lint = [&](const nlohmann::json& preset_slice) {
                try {
                    return (*model)->LintSlices(global, preset_slice);
                } catch (...) {
                    return std::vector<std::string>{};  // a lint is never worth a startup failure
                }
            };
            if (!global.is_null()) {
                for (const auto& finding : lint(nullptr)) {
                    results.push_back(
                        Warning("naming_model", "permissions.json plugins." + name + ": " + finding));
                }
            }
            for (const auto& [preset_name, raw] : presets_) {
                if (!raw.plugin_permissions.contains(name)) continue;
                for (const auto& finding : lint(raw.plugin_permissions.at(name))) {
                    results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                                                  "': plugin_permissions." + name +
                                                                  ": " + finding));
                }
            }
        }
    }

    // Permission slices (permissions.json `plugins`, preset
    // `plugin_permissions`) must name a registered permission model: a slice
    // for a plugin that is disabled or misspelled governs nothing.
    for (const auto& [name, slice] : bundle_.permissions.plugin_slices.items()) {
        (void)slice;
        if (!has_model(name)) {
            results.push_back(Warning("naming_model", "permissions.json plugins." + name +
                                                          " names no registered permission model "
                                                          "(typo, or the providing plugin is disabled)"));
        }
    }
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& [name, slice] : raw.plugin_permissions.items()) {
            (void)slice;
            if (!has_model(name)) {
                results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                                              "': plugin_permissions." + name +
                                                              " names no registered permission model "
                                                              "(typo, or the providing plugin is disabled)"));
            }
        }
    }

    // tool_config namespace keys must name a namespace some registered tool
    // claims: a typo or a slice
    // stranded by a disabled plugin is silent data -- no tool will ever read
    // it -- and must be loud at startup.
    const auto claimed_namespaces = inputs_.registries.tools->ConfigNamespaces();
    for (const auto& [preset_name, raw] : presets_) {
        for (const auto& [ns, value] : raw.tool_config.items()) {
            (void)value;
            if (!claimed_namespaces.owners.count(ns)) {
                results.push_back(Warning("naming_model", "agent preset '" + preset_name +
                                      "': tool_config namespace '" + ns +
                                      "' matches no registered tool (typo, or the providing plugin is disabled)"));
            }
        }
    }

    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_advisor() {
    std::vector<CheckResult> results;
    if (bundle_.permissions.advisor) {
        const bool resolved = inputs_.registries.permission_advisors != nullptr &&
                              inputs_.registries.permission_advisors->Has(*bundle_.permissions.advisor);
        if (!resolved) {
            results.push_back(Warning(
                "advisor", "permissions.json names advisor '" + *bundle_.permissions.advisor +
                               "' but no loaded plugin registered one (typo, or the providing "
                               "plugin is disabled); prompts fall through to the human"));
        }
    } else if (bundle_.permissions.advisor_inherit) {
        results.push_back(Warning(
            "advisor", "permissions.json sets advisor_inherit but names no advisor; "
                       "the flag is inert"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_compaction_strategy() {
    std::vector<CheckResult> results;
    if (!bundle_.settings_loaded) return results;  // already Fatal under config_parse

    const std::string& name = bundle_.settings.memory.compaction_strategy;
    // "summarize" is core's own built-in, registered directly in main.cpp
    // before plugin load -- it resolves even when
    // inputs_.registries.compaction_strategies is null (a test harness that
    // never wires the registry).
    const bool resolved =
        name == "summarize" ||
        (inputs_.registries.compaction_strategies != nullptr && inputs_.registries.compaction_strategies->Has(name));
    if (!resolved) {
        results.push_back(Warning(
            "compaction_strategy", "settings.json memory.compaction_strategy names '" + name +
                                       "' but no loaded plugin registered it (typo, or the "
                                       "providing plugin is disabled); compaction falls back to "
                                       "the built-in 'summarize' strategy"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_compaction_keep_recent() {
    std::vector<CheckResult> results;
    if (!bundle_.settings_loaded) return results;  // already Fatal under config_parse

    const auto keep_recent = bundle_.settings.memory.compaction_keep_recent_tokens;
    if (keep_recent <= 0) return results;  // 0 = unset (replace_percent governs); nothing to compare

    // Only FixedTokens has a value comparable without a session's resolved
    // model: PercentOfWindow and WindowBuffer resolve against
    // context_window_tokens, a property of whichever model a session ends
    // up running -- not decidable from settings.json alone at startup.
    const auto& trigger = bundle_.settings.memory.context_compaction_trigger;
    if (trigger.kind == CompactionTriggerSettings::Kind::FixedTokens &&
        keep_recent >= trigger.tokens) {
        results.push_back(Warning(
            "compaction_keep_recent_tokens",
            "settings.json memory.compaction_keep_recent_tokens (" + std::to_string(keep_recent) +
                ") is not smaller than the fixed compaction trigger (" +
                std::to_string(trigger.tokens) +
                " tokens); the compaction strategy's cut selector will cover the whole working "
                "set every time it fires, degrading auto-compaction into per-turn tool-result "
                "elision instead of an actual fold"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_small_model() {
    std::vector<CheckResult> results;
    if (!bundle_.settings_loaded) return results;  // already Fatal under config_parse

    const std::string& ref_text = bundle_.settings.small_model;
    if (ref_text.empty()) return results;  // compact on the session's own model: normal

    // Same shape check SessionRegistry::ApplySmallModel applies at session
    // creation (SplitModelRef, so a discovered catalog's slash-bearing model
    // id is accepted here too) -- run here so the misconfiguration surfaces at
    // startup with the rest, not on the first compaction of the first long
    // session.
    const auto ref = common::SplitModelRef(ref_text);
    if (!ref) {
        results.push_back(
            Warning("small_model", "settings.json small_model is '" + ref_text +
                                       "'; not a <provider>/<model> reference -- compaction "
                                       "behaves as if it were unset"));
        return results;
    }
    if (!bundle_.providers_loaded) return results;  // providers.json already Fatal under config_parse
    if (bundle_.providers.Resolve(*ref) == nullptr) {
        results.push_back(Warning("small_model", "settings.json small_model '" + ref_text +
                                      "' resolves to nothing in providers.json; compaction "
                                      "behaves as if it were unset"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_default_subagent_model() {
    std::vector<CheckResult> results;
    if (!bundle_.settings_loaded) return results;  // already Fatal under config_parse

    const std::string& ref_text = bundle_.settings.default_subagent_model;
    if (ref_text.empty()) return results;  // children inherit the parent's model: normal

    // Same shape check the spawn path applies (SplitModelRef, so a discovered
    // catalog's slash-bearing model id is accepted here too) -- run here so the
    // misconfiguration surfaces at startup, not on the first spawn.
    const auto ref = common::SplitModelRef(ref_text);
    if (!ref) {
        results.push_back(Warning(
            "default_subagent_model",
            "settings.json default_subagent_model is '" + ref_text +
                "'; not a <provider>/<model> reference -- subagents behave as if it were unset"));
        return results;
    }
    if (!bundle_.providers_loaded) return results;  // providers.json already Fatal under config_parse
    if (bundle_.providers.Resolve(*ref) == nullptr) {
        results.push_back(Warning("default_subagent_model",
                                  "settings.json default_subagent_model '" + ref_text +
                                      "' resolves to nothing in providers.json; subagents "
                                      "behave as if it were unset"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_subagent_limit() {
    std::vector<CheckResult> results;
    if (!bundle_.settings_loaded) return results;  // already Fatal under config_parse
    const auto limit = bundle_.settings.max_concurrent_subagents;
    if (limit <= 0) {
        results.push_back(Warning("subagent_limit", "settings.json max_concurrent_subagents is " +
                                 std::to_string(limit) + "; spawn_subagent will always fail"));
    } else if (limit > 8) {
        // Warns, does not block: past 8 the task has outgrown the harness, but
        // that is the operator's call (ARCHITECTURE.md, "Subagents").
        results.push_back(Warning("subagent_limit", "settings.json max_concurrent_subagents is " +
                                 std::to_string(limit) + "; past 8 the task has outgrown the harness"));
    }
    return results;
}

std::vector<CheckResult> StartupSanityCheck::check_hooks() {
    std::vector<CheckResult> results;
    if (bundle_.hooks.hooks.empty()) return results;  // no hooks.json, or none -- both normal

    // Structural warnings that do not need the point registry.
    for (const auto& action : bundle_.hooks.hooks) {
        if (action.command.find_first_not_of(" \t\n") == std::string::npos) {
            results.push_back(Warning("hooks", "action on '" + action.on +
                                  "': command is whitespace only and will fail open"));
        }
    }

    // Point-relative warnings. These need the dispatcher because the point is
    // code -- core's or a plugin's -- and the check is deliberately structural:
    // it reads which points REGISTERED, never fires one. Run before Configure,
    // whose own warnings are the runtime backstop. Shared with Configure via
    // DescribeActionWarnings so the two can never drift.
    if (inputs_.registries.hooks == nullptr) return results;

    for (const auto& action : bundle_.hooks.hooks) {
        for (const auto& warning : inputs_.registries.hooks->DescribeActionWarnings(action)) {
            results.push_back(Warning("hooks", warning));
        }
    }
    return results;
}

std::unique_ptr<session::SessionDatabase> OpenSessionDatabase(const ConfigBundle& bundle,
                                                              std::vector<CheckResult>& problems) {
    // A settings.json that failed to load falls back to the default path: the
    // config_parse Fatal already names that problem, and this one reports
    // whether the database itself is sound.
    const auto db_path = session::ResolveSessionDbPath(
        bundle.settings_loaded ? bundle.settings.session_db_path : std::string{});
    try {
        return std::make_unique<session::SessionDatabase>(db_path);
    } catch (const std::exception& e) {
        problems.push_back(
            Fatal("session_db", "session database '" + db_path.string() + "': " + e.what()));
        return nullptr;
    }
}

}  // namespace minimal::startup
