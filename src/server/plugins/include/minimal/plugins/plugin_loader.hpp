#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/plugins/service_registry.hpp"
#include "minimal/rpc/auth_provider.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

class PluginStorage;  // forwarded; the full type reaches plugins via plugin_abi.h

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
                 agent::MentionResolverRegistry* mention_resolvers = nullptr);
    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Loads every enabled candidate. One plugin's failure never aborts the
    // others: every outcome, good or bad, comes back as a PluginRecord.
    std::vector<PluginRecord> LoadAll(const std::vector<PluginCandidate>& candidates);

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
    std::vector<void*> handles_;  // never closed; see LIFETIME above
};

}  // namespace minimal::plugins
