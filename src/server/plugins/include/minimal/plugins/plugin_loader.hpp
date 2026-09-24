#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_contributor.hpp"
#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/metadata_catalog.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/llm/variant_params.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/rpc/auth_provider.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

class PluginStorage;  // forwarded; the full type reaches plugins via plugin_abi.h
// Defined in plugin_loader.cpp: holds a copy of a project-command plugin's
// own PluginRegistrationContext (plugin_abi.h), which this header does not
// otherwise need to see in full -- see project_command_plugins_ below.
struct ProjectCommandsPlugin;

// "Came out of this build" vs. "someone dropped this in" is a real trust
// distinction, and it drives both the default enablement and the severity of a
// load failure -- bundled failures are fatal (a harness without tools-fs is
// useless, not degraded), user failures are warnings.
enum class Provenance { Bundled, User };

enum class LoadStatus {
    Loaded,
    Disabled,               // plugins.json says enabled:false; never opened
    OpenFailed,             // dlopen failed (missing file, bad arch, unresolved symbol)
    MissingRequiredSymbol,  // not a minimal plugin, or built against a different ABI header
    AbiMismatch,            // built against a different toolchain; rebuild it
    // A registration callback threw. Plugin code is arbitrary native code by
    // design, so a throw is a load outcome like any other -- it used to
    // propagate through LoadAll into main(), which has no handler, killing the
    // process without naming the plugin responsible.
    RegistrationThrew,
};

struct PluginCandidate {
    std::filesystem::path file;
    Provenance provenance = Provenance::User;
    bool enabled = false;
    // The candidate's own config from its plugins.json entry, verbatim
    // (PluginConfig::Entry::config, propagated by DiscoverPlugins). Forwarded
    // to the plugin's registration hooks; core imposes no schema on it.
    nlohmann::json config = nlohmann::json::object();
};

struct PluginRecord {
    std::filesystem::path file;
    std::string name;
    std::string version;
    std::string abi_tag;
    Provenance provenance = Provenance::User;
    LoadStatus status = LoadStatus::OpenFailed;
    std::string error;  // human-readable; empty when Loaded or Disabled

    // ORIGINAL names, as the plugin declared them -- not surface names. A tool
    // that was aliased at registration is still listed under the name it
    // registered with, because collision detection and permission keys are
    // computed from "who provided what", which a rename does not change.
    std::vector<std::string> registered_tools;
    std::vector<std::string> registered_provider_kinds;
    std::vector<std::string> registered_commands;  // surface names, after aliasing
    bool registered_session_observer = false;
    bool registered_auth_provider = false;
    bool registered_mention_resolver = false;
    bool registered_context_contributor = false;
    // True when the plugin registered an advisor that actually landed (a
    // duplicate from the same plugin is refused and does not count twice).
    bool registered_permission_advisor = false;
    // ORIGINAL service names, as the plugin declared them. Services live in
    // their own namespace -- no aliasing, no permission keys.
    std::vector<std::string> registered_services;
    // Catalog kinds, keyed like registered_provider_kinds (the same kind string
    // a providers.json `backend` names). Feeds the sanity check's discover-entry
    // warnings and its duplicate-catalog report.
    std::vector<std::string> registered_model_catalog_kinds;
    // Enrichment-catalog kinds, keyed like registered_model_catalog_kinds (a
    // models.dev-style default source registers under the wire kinds it
    // knows). Feeds the startup print and the sanity check's metadata report.
    std::vector<std::string> registered_metadata_catalog_kinds;
    // True when the plugin appended a fallback enrichment catalog (answers for
    // kinds nobody named). Recorded for the same diagnostics.
    bool registered_metadata_fallback = false;
    // Compaction strategy kinds, keyed like registered_provider_kinds. Feeds
    // the sanity check's memory.compaction_strategy resolution warning.
    std::vector<std::string> registered_compaction_strategy_kinds;
    // Variant-params kinds, keyed like registered_provider_kinds (the same
    // kind string a providers.json `backend` names). Feeds the startup print;
    // an unregistered kind is not an error -- it just means that backend uses
    // the generic reasoning_effort fallback.
    std::vector<std::string> registered_variant_params_kinds;
    // FULL hook point names, as the plugin declared them. Points live in their
    // own namespace -- no aliasing, no permission keys -- and a duplicate is
    // fatal at startup via the NameRegistry (NameKind::HookPoint).
    std::vector<std::string> registered_hook_points;

    bool ok() const { return status == LoadStatus::Loaded; }
};

std::string_view ToString(LoadStatus status);

// Opens plugins and invokes whichever registration seams they export.
//
// LIFETIME: handles are held until the process exits and are deliberately never
// dlclose'd. A registered Tool's vtable lives inside its .so, so unloading a
// plugin while the registry still holds its objects is a use-after-free that
// surfaces as an unrelated crash much later. Do not "clean this up".
class PluginLoader {
public:
    // `tools_settings` is the `tools` object from settings.json, forwarded
    // verbatim to every plugin's tool registration. Core imposes no schema on
    // it; each plugin owns the shape of its own slice.
    // `observers` is a pointer, not a reference, because it is genuinely
    // optional: a test that only exercises tool or provider registration has no
    // session lifecycle to observe, and passing a throwaway registry there would
    // spin up a thread pool for nothing. Null means the seam is simply not
    // offered -- plugins already treat every registration hook as optional.
    PluginLoader(tools::ToolRegistry& tools, llm::ProviderFactoryRegistry& providers,
                 nlohmann::json tools_settings = nlohmann::json::object(),
                 agent::SessionObserverRegistry* observers = nullptr,
                 commands::CommandRegistry* commands = nullptr,
                 rpc::AuthProviderRegistry* auth_providers = nullptr,
                 tools::PermissionAdvisorRegistry* permission_advisors = nullptr,
                 llm::ModelCatalogRegistry* model_catalogs = nullptr,
                 ServiceRegistry* services = nullptr,
                 PluginStorage* storage = nullptr,
                 std::filesystem::path plugin_data_root = {},
                 hooks::HookDispatcher* hook_points = nullptr,
                 agent::MentionResolverRegistry* mention_resolvers = nullptr,
                 agent::CompactionStrategyRegistry* compaction_strategies = nullptr,
                 llm::MetadataCatalogRegistry* metadata_catalogs = nullptr,
                 llm::VariantParamsRegistry* variant_params = nullptr,
                 agent::ContextContributorRegistry* context_contributors = nullptr);
    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Loads every enabled candidate. One plugin's failure never aborts the
    // others: every outcome, good or bad, comes back as a PluginRecord.
    std::vector<PluginRecord> LoadAll(const std::vector<PluginCandidate>& candidates);

    // Invokes every loaded plugin's OPTIONAL
    // minimal_plugin_register_project_commands hook against `registry` for
    // `project_root`. Called lazily, per root, by
    // commands::ProjectCommandSource -- never at startup. A throwing hook is
    // caught and logged here (not rolled back: `registry` is a fresh,
    // throwaway per-root instance the caller discards on failure, not the
    // global one startup's rollback exists to protect). A no-op if `commands`
    // was never given to the constructor (no CommandRegistry* means no plugin
    // could have exported the symbol in the first place -- see LoadOne).
    void RegisterProjectCommands(commands::CommandRegistry& registry,
                                 const std::filesystem::path& project_root) const;

private:
    PluginRecord LoadOne(const PluginCandidate& candidate);

    tools::ToolRegistry& tools_;
    llm::ProviderFactoryRegistry& providers_;
    nlohmann::json tools_settings_;
    agent::SessionObserverRegistry* observers_ = nullptr;
    // Optional like the others; null means plugins cannot register a model
    // catalog. Additive across plugins, keyed by the provider kind string;
    // a duplicate kind is refused and reported by the sanity check.
    llm::ModelCatalogRegistry* model_catalogs_ = nullptr;
    // Optional for the same reason observers_ is: a test exercising only tool
    // registration has no command surface to offer, and building one would be
    // scaffolding for nothing.
    commands::CommandRegistry* commands_ = nullptr;
    // Optional like the others; null means plugins cannot register an auth
    // provider. Exclusive registry: the first plugin to register wins.
    rpc::AuthProviderRegistry* auth_providers_ = nullptr;
    // Optional for the same reason again: the seam is exclusive (first plugin
    // wins) and the registry itself carries the core default, so null just
    // means plugins cannot override the resolver.
    agent::MentionResolverRegistry* mention_resolvers_ = nullptr;
    // Optional like the others; null means plugins cannot pre-fill session
    // context. Additive, with a removal seam (count snapshot) so a throwing
    // plugin's contributors are rolled back like its observers.
    agent::ContextContributorRegistry* context_contributors_ = nullptr;
    // Optional like the others; null means plugins cannot register a permission
    // advisor. Additive across plugins; permissions.json selects one by name.
    tools::PermissionAdvisorRegistry* permission_advisors_ = nullptr;
    // Optional like the others; null means plugins cannot register services.
    // Additive across plugins; duplicate names are fatal at startup via the
    // NameRegistry (a new kind), not shadowed here. Also exposed to every
    // plugin through PluginRegistrationContext::services as the CONSUMER side.
    ServiceRegistry* services_ = nullptr;
    // Optional like the others; null means no per-plugin KV storage is offered
    // and PluginRegistrationContext::storage is null -- the plugin degrades.
    // Owned by main (process lifetime, like the registries); the loader only
    // forwards the pointer.
    PluginStorage* storage_ = nullptr;
    // Root under which each plugin's own data_dir is provisioned
    // (<root>/<plugin_name>/, created before its registration hooks run).
    // Empty disables provisioning.
    std::filesystem::path plugin_data_root_;
    // Optional like the others; null means plugins cannot register hook points.
    // Additive across plugins; duplicate point names are fatal at startup via
    // the NameRegistry (NameKind::HookPoint). The same object is the dispatch
    // surface the plugin's HookPoint handles emit through, which is why it is
    // one registry-and-dispatcher rather than two.
    hooks::HookDispatcher* hook_points_ = nullptr;
    // Optional like the others; null means plugins cannot register a
    // compaction strategy. Additive across plugins, keyed by name;
    // memory.compaction_strategy in settings.json selects one. Core's own
    // built-in "summarize" strategy is registered directly in main.cpp
    // before plugin load, not through this hook.
    agent::CompactionStrategyRegistry* compaction_strategies_ = nullptr;
    // Optional like the others; null means plugins cannot register an
    // enrichment (metadata) catalog. Additive across plugins, keyed by the
    // provider kind string with a first-answering fallback chain for kinds
    // nobody named; a duplicate kind is refused and reported by the sanity
    // check. Beside model_catalogs_, not merged with it: the kind slots there
    // are the listing catalogs', and this seam's contract is per-model Lookup
    // with a cache, not a pure List.
    llm::MetadataCatalogRegistry* metadata_catalogs_ = nullptr;
    // Optional like the others; null means plugins cannot register a
    // variant-params provider. Additive across plugins, keyed by the provider
    // kind string; a duplicate kind is refused and reported by the sanity
    // check. An unregistered kind is not a degraded state -- it is the
    // ordinary case for every backend that needs no per-kind wire shape.
    llm::VariantParamsRegistry* variant_params_ = nullptr;
    std::vector<void*> handles_;  // never closed; see LIFETIME above

    // One entry per loaded plugin that exports
    // minimal_plugin_register_project_commands, checked once at LoadOne time
    // (cheap: a dlsym, not a call). Each entry carries a COPY of what this
    // plugin's other hooks saw at startup -- config, data_dir, tools_settings,
    // the services/storage pointers -- because RegisterProjectCommands runs
    // long after LoadOne returns, per project root, and LoadOne's own
    // `context` local does not survive that. unique_ptr of the
    // forward-declared type (defined in plugin_loader.cpp, alongside
    // PluginRegistrationContext's only other use in this header's TU) is what
    // lets this header avoid including all of plugin_abi.h just to name a
    // member type nothing else here needs; valid because ~PluginLoader is
    // itself defined out-of-line, in the same TU that completes the type.
    std::vector<std::unique_ptr<ProjectCommandsPlugin>> project_command_plugins_;
};

}  // namespace minimal::plugins
