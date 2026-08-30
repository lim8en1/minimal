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
using RegisterToolsFn = void (*)(tools::ToolRegistry&, const ToolRegistrationContext&);
using RegisterProviderFactoriesFn = void (*)(llm::ProviderFactoryRegistry&);
using RegisterSessionObserverFn = void (*)(agent::SessionObserverRegistry&);

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
                            agent::SessionObserverRegistry* observers)
    : tools_(tools),
      providers_(providers),
      tools_settings_(std::move(tools_settings)),
      observers_(observers) {}

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

    // Attribution window: every tool registered inside it belongs to this
    // plugin, which is what lets a later collision name both culprits.
    const std::size_t tools_before = tools_.attributions().size();
    tools_.SetAttribution(record.name);

    if (auto register_tools = Lookup<RegisterToolsFn>(handle, "minimal_plugin_register_tools")) {
        ToolRegistrationContext context;
        context.tools_settings = tools_settings_;
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
        register_providers(providers_);
        const auto after = providers_.kinds();
        std::set_difference(after.begin(), after.end(), before.begin(), before.end(),
                            std::back_inserter(record.registered_provider_kinds));
    }

    if (observers_ != nullptr) {
        if (auto register_observer = Lookup<RegisterSessionObserverFn>(
                handle, "minimal_plugin_register_session_observer")) {
            register_observer(*observers_);
            record.registered_session_observer = true;
        }
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
