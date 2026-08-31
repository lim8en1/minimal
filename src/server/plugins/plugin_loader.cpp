#include "minimal/plugins/plugin_loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <iterator>

#include "minimal/common/abi_tag.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace minimal::plugins {
namespace {

using AbiTagFn = const char* (*)();
using NameFn = const char* (*)();
using VersionFn = const char* (*)();
using RegisterToolsFn =
    void (*)(tools::ToolRegistry&, const PluginRegistrationContext&);
using RegisterProviderFactoriesFn =
    void (*)(llm::ProviderFactoryRegistry&, const PluginRegistrationContext&);
using RegisterSessionObserverFn =
    void (*)(agent::SessionObserverRegistry&, const PluginRegistrationContext&);
using RegisterCommandsFn =
    void (*)(commands::CommandRegistry&, const PluginRegistrationContext&);
using RegisterAuthProviderFn =
    void (*)(rpc::AuthProviderRegistry&, const PluginRegistrationContext&);
using RegisterPermissionAdvisorFn =
    void (*)(tools::PermissionAdvisorRegistry&, const PluginRegistrationContext&);
using RegisterModelCatalogsFn =
    void (*)(llm::ModelCatalogRegistry&, const PluginRegistrationContext&);
using RegisterServicesFn =
    void (*)(ServiceRegistry&, const PluginRegistrationContext&);

template <typename Fn>
Fn Lookup(void* handle, const char* symbol) {
    // Cast through void* rather than reinterpret_cast'ing the dlsym result
    // directly: converting object pointers to function pointers is conditionally
    // supported in C++, and this is the form POSIX documents.
    void* address = dlsym(handle, symbol);
    Fn fn = nullptr;
    static_assert(sizeof(fn) == sizeof(address), "function/object pointer size mismatch");
    std::memcpy(&fn, &address, sizeof(fn));
    return fn;
}

// Runs a plugin's registration callback with the attribution window open,
// closing it and reporting on either exit.
//
// PLUGIN CODE IS NOT TRUSTED TO RETURN NORMALLY. These callbacks were called
// bare: a throw propagated out of LoadOne, out of LoadAll, into main() -- which
// has no handler -- so the process died with no diagnostic naming the plugin,
// and the attribution window stayed open, mis-attributing every registration
// after it to a plugin that had already failed. Every OTHER plugin entry point
// in this tree is wrapped (observers in Dispatch, commands in Dispatch, tools in
// ToolExecutor::Run); registration was the one that was not.
template <typename Open, typename Body, typename Close>
bool GuardedRegistration(PluginRecord& record, const char* what, Open&& open, Body&& body,
                          Close&& close) {
    open();
    try {
        body();
    } catch (const std::exception& error) {
        close();
        record.status = LoadStatus::RegistrationThrew;
        record.error = std::string(what) + " threw: " + error.what();
        return false;
    } catch (...) {
        close();
        record.status = LoadStatus::RegistrationThrew;
        record.error = std::string(what) + " threw a non-standard exception";
        return false;
    }
    close();
    return true;
}

}  // namespace

std::string_view ToString(LoadStatus status) {
    switch (status) {
        case LoadStatus::Loaded:                return "loaded";
        case LoadStatus::Disabled:              return "disabled";
        case LoadStatus::OpenFailed:            return "open failed";
        case LoadStatus::MissingRequiredSymbol: return "missing required symbol";
        case LoadStatus::AbiMismatch:           return "abi mismatch";
        case LoadStatus::RegistrationThrew:     return "registration threw";
    }
    return "unknown";
}

PluginLoader::PluginLoader(tools::ToolRegistry& tools, llm::ProviderFactoryRegistry& providers,
                            nlohmann::json tools_settings,
                            agent::SessionObserverRegistry* observers,
                           commands::CommandRegistry* commands,
                           rpc::AuthProviderRegistry* auth_providers,
                           tools::PermissionAdvisorRegistry* permission_advisors,
                           llm::ModelCatalogRegistry* model_catalogs,
                           ServiceRegistry* services)
    : tools_(tools),
      providers_(providers),
      tools_settings_(std::move(tools_settings)),
      observers_(observers),
      model_catalogs_(model_catalogs),
      commands_(commands),
      auth_providers_(auth_providers),
      permission_advisors_(permission_advisors),
      services_(services) {}

// Handles are intentionally leaked -- see LIFETIME in the header.
PluginLoader::~PluginLoader() = default;

PluginRecord PluginLoader::LoadOne(const PluginCandidate& candidate) {
    PluginRecord record;
    record.file = candidate.file;
    record.provenance = candidate.provenance;

    if (!candidate.enabled) {
        record.status = LoadStatus::Disabled;
        return record;
    }

    // RTLD_NOW so an unresolved symbol surfaces here, with a name attached,
    // rather than as a crash the first time that code path runs.
    // RTLD_LOCAL keeps this plugin's symbols out of the global namespace; it does
    // not prevent resolving *host* symbols, which is what plugins actually need.
    void* handle = dlopen(candidate.file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        record.status = LoadStatus::OpenFailed;
        const char* message = dlerror();
        record.error = message ? message : "dlopen failed";
        return record;
    }
    handles_.push_back(handle);

    auto abi_tag_fn = Lookup<AbiTagFn>(handle, "minimal_plugin_abi_tag");
    auto name_fn = Lookup<NameFn>(handle, "minimal_plugin_name");
    auto version_fn = Lookup<VersionFn>(handle, "minimal_plugin_version");
    if (abi_tag_fn == nullptr || name_fn == nullptr || version_fn == nullptr) {
        record.status = LoadStatus::MissingRequiredSymbol;
        record.error = "not a minimal plugin: missing minimal_plugin_abi_tag/name/version";
        return record;
    }

    record.abi_tag = abi_tag_fn();
    record.name = name_fn();
    record.version = version_fn();

    // Checked before any registration callback runs: a plugin built against a
    // different toolchain must never get to execute against our types.
    if (record.abi_tag != std::string(common::AbiTag())) {
        record.status = LoadStatus::AbiMismatch;
        record.error = "built for '" + record.abi_tag + "', host is '" +
                        std::string(common::AbiTag()) + "' -- rebuild the plugin";
        return record;
    }

    // Everything the registration hooks see: the shared settings.json `tools`
    // object plus THIS plugin's own plugins.json `config` slice. Built once per
    // plugin, before any hook runs.
    PluginRegistrationContext context;
    context.tools_settings = tools_settings_;
    context.config = candidate.config;
    // The consumer side of the service channel: the same registry that
    // register_services below dispatches into, so a plugin capturing this
    // pointer at registration resolves services from this process's registry.
    context.services = services_;

    // Attribution window: every tool registered inside it belongs to this
    // plugin, which is what lets a later collision name both culprits.
    const std::size_t tools_before = tools_.attributions().size();
    if (auto register_tools = Lookup<RegisterToolsFn>(handle, "minimal_plugin_register_tools")) {
        const bool ok = GuardedRegistration(
            record, "minimal_plugin_register_tools",
            [&] { tools_.SetAttribution(record.name); },
            [&] { register_tools(tools_, context); },
            [&] { tools_.ClearAttribution(); });
        if (!ok) return record;
    }

    const auto& attributions = tools_.attributions();
    for (std::size_t i = tools_before; i < attributions.size(); ++i) {
        record.registered_tools.push_back(attributions[i].tool_name);
    }

    if (auto register_providers = Lookup<RegisterProviderFactoriesFn>(
            handle, "minimal_plugin_register_provider_factories")) {
        const auto before = providers_.kinds();
        const bool ok = GuardedRegistration(
            record, "minimal_plugin_register_provider_factories",
            [&] { providers_.SetAttribution(record.name); },
            [&] { register_providers(providers_, context); },
            [&] { providers_.ClearAttribution(); });
        if (!ok) return record;
        const auto after = providers_.kinds();
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                            std::back_inserter(record.registered_provider_kinds));
    }

    if (commands_ != nullptr) {
        // Same attribution window as tools, and for the same two reasons: a
        // collision must be able to name both plugins, and an alias is keyed by
        // "plugin.original_name" so the registry has to know whose it is.
        const std::size_t commands_before = commands_->attributions().size();
        if (auto register_commands =
                Lookup<RegisterCommandsFn>(handle, "minimal_plugin_register_commands")) {
            const bool ok = GuardedRegistration(
                record, "minimal_plugin_register_commands",
                [&] { commands_->SetAttribution(record.name); },
                [&] { register_commands(*commands_, context); },
                [&] { commands_->ClearAttribution(); });
            if (!ok) return record;
        }

        const auto& command_attributions = commands_->attributions();
        for (std::size_t i = commands_before; i < command_attributions.size(); ++i) {
            record.registered_commands.push_back(command_attributions[i].surface_name);
        }
    }

    if (observers_ != nullptr) {
        if (auto register_observer = Lookup<RegisterSessionObserverFn>(
                handle, "minimal_plugin_register_session_observer")) {
            if (!GuardedRegistration(record, "minimal_plugin_register_session_observer",
                                      [] {}, [&] { register_observer(*observers_, context); },
                                      [] {})) {
                return record;
            }
            record.registered_session_observer = true;
        }
    }

    if (auth_providers_ != nullptr) {
        if (auto register_auth = Lookup<RegisterAuthProviderFn>(
                handle, "minimal_plugin_register_auth_provider")) {
            if (!GuardedRegistration(record, "minimal_plugin_register_auth_provider", [] {},
                                      [&] { register_auth(*auth_providers_, context); }, [] {})) {
                return record;
            }
            record.registered_auth_provider = true;
        }
    }

    if (permission_advisors_ != nullptr) {
        // Same attribution window as tools and commands: registrations arriving
        // inside it are keyed by this plugin's name, which is what
        // permissions.json's "advisor" field selects on.
        const std::size_t advisors_before = permission_advisors_->attributions().size();
        if (auto register_advisor = Lookup<RegisterPermissionAdvisorFn>(
                handle, "minimal_plugin_register_permission_advisor")) {
            if (!GuardedRegistration(record, "minimal_plugin_register_permission_advisor",
                                      [&] { permission_advisors_->SetAttribution(record.name); },
                                      [&] { register_advisor(*permission_advisors_, context); },
                                      [&] { permission_advisors_->ClearAttribution(); })) {
                return record;
            }
        }
        record.registered_permission_advisor =
            permission_advisors_->attributions().size() > advisors_before;
    }

    if (model_catalogs_ != nullptr) {
        // Same attribution window as tools and commands: a kind two plugins
        // both register must be diagnosable down to both, which the
        // attributions feed the sanity check for.
        const auto kinds_before = model_catalogs_->kinds();
        if (auto register_catalogs = Lookup<RegisterModelCatalogsFn>(
                handle, "minimal_plugin_register_model_catalogs")) {
            if (!GuardedRegistration(record, "minimal_plugin_register_model_catalogs", [] {},
                                      [&] { register_catalogs(*model_catalogs_, context); }, [] {})) {
                return record;
            }
        }
        const auto kinds_after = model_catalogs_->kinds();
        std::set_difference(kinds_after.begin(), kinds_after.end(), kinds_before.begin(),
                            kinds_before.end(), std::back_inserter(record.registered_model_catalog_kinds));
    }

    record.status = LoadStatus::Loaded;

    if (services_ != nullptr) {
        // Same attribution window as tools and commands: a duplicate service
        // name must be diagnosable down to BOTH plugins, which is what the
        // (plugin, name) pairs feed NameRegistry.
        const std::size_t services_before = services_->attributions().size();
        if (auto register_services = Lookup<RegisterServicesFn>(
                handle, "minimal_plugin_register_services")) {
            if (!GuardedRegistration(record, "minimal_plugin_register_services",
                                      [&] { services_->SetAttribution(record.name); },
                                      [&] { register_services(*services_, context); },
                                      [&] { services_->ClearAttribution(); })) {
                return record;
            }
        }

        const auto& service_attributions = services_->attributions();
        for (std::size_t i = services_before; i < service_attributions.size(); ++i) {
            record.registered_services.push_back(service_attributions[i].service_name);
        }
    }

    return record;
}

std::vector<PluginRecord> PluginLoader::LoadAll(const std::vector<PluginCandidate>& candidates) {
    std::vector<PluginRecord> records;
    records.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        records.push_back(LoadOne(candidate));
    }
    return records;
}

}  // namespace minimal::plugins
