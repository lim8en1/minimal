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

}  // namespace

std::string_view ToString(LoadStatus status) {
    switch (status) {
        case LoadStatus::Loaded:                return "loaded";
        case LoadStatus::Disabled:              return "disabled";
        case LoadStatus::OpenFailed:            return "open failed";
        case LoadStatus::MissingRequiredSymbol: return "missing required symbol";
        case LoadStatus::AbiMismatch:           return "abi mismatch";
    }
    return "unknown";
}

PluginLoader::PluginLoader(tools::ToolRegistry& tools, llm::ProviderFactoryRegistry& providers,
                            nlohmann::json tools_settings,
                            agent::SessionObserverRegistry* observers,
                           commands::CommandRegistry* commands,
                           rpc::AuthProviderRegistry* auth_providers,
                           tools::PermissionAdvisorRegistry* permission_advisors)
    : tools_(tools),
      providers_(providers),
      tools_settings_(std::move(tools_settings)),
      observers_(observers),
      commands_(commands),
      auth_providers_(auth_providers),
      permission_advisors_(permission_advisors) {}

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

    // Attribution window: every tool registered inside it belongs to this
    // plugin, which is what lets a later collision name both culprits.
    const std::size_t tools_before = tools_.attributions().size();
    tools_.SetAttribution(record.name);

    if (auto register_tools = Lookup<RegisterToolsFn>(handle, "minimal_plugin_register_tools")) {
        register_tools(tools_, context);
    }

    tools_.ClearAttribution();

    const auto& attributions = tools_.attributions();
    for (std::size_t i = tools_before; i < attributions.size(); ++i) {
        record.registered_tools.push_back(attributions[i].tool_name);
    }

    if (auto register_providers = Lookup<RegisterProviderFactoriesFn>(
            handle, "minimal_plugin_register_provider_factories")) {
        const auto before = providers_.kinds();
        register_providers(providers_, context);
        const auto after = providers_.kinds();
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                            std::back_inserter(record.registered_provider_kinds));
    }

    if (commands_ != nullptr) {
        // Same attribution window as tools, and for the same two reasons: a
        // collision must be able to name both plugins, and an alias is keyed by
        // "plugin.original_name" so the registry has to know whose it is.
        const std::size_t commands_before = commands_->attributions().size();
        commands_->SetAttribution(record.name);
        if (auto register_commands =
                Lookup<RegisterCommandsFn>(handle, "minimal_plugin_register_commands")) {
            register_commands(*commands_, context);
        }
        commands_->ClearAttribution();

        const auto& command_attributions = commands_->attributions();
        for (std::size_t i = commands_before; i < command_attributions.size(); ++i) {
            record.registered_commands.push_back(command_attributions[i].surface_name);
        }
    }

    if (observers_ != nullptr) {
        if (auto register_observer = Lookup<RegisterSessionObserverFn>(
                handle, "minimal_plugin_register_session_observer")) {
            register_observer(*observers_, context);
            record.registered_session_observer = true;
        }
    }

    if (auth_providers_ != nullptr) {
        if (auto register_auth = Lookup<RegisterAuthProviderFn>(
                handle, "minimal_plugin_register_auth_provider")) {
            register_auth(*auth_providers_, context);
            record.registered_auth_provider = true;
        }
    }

    if (permission_advisors_ != nullptr) {
        // Same attribution window as tools and commands: registrations arriving
        // inside it are keyed by this plugin's name, which is what
        // permissions.json's "advisor" field selects on.
        const std::size_t advisors_before = permission_advisors_->attributions().size();
        permission_advisors_->SetAttribution(record.name);
        if (auto register_advisor = Lookup<RegisterPermissionAdvisorFn>(
                handle, "minimal_plugin_register_permission_advisor")) {
            register_advisor(*permission_advisors_, context);
        }
        permission_advisors_->ClearAttribution();
        record.registered_permission_advisor =
            permission_advisors_->attributions().size() > advisors_before;
    }

    record.status = LoadStatus::Loaded;
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
