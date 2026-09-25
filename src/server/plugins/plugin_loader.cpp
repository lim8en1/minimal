#include "minimal/plugins/plugin_loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <functional>
#include <iterator>

#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/plugins/plugin_abi.h"

namespace minimal::plugins {

// Completes the type plugin_loader.hpp forward-declares. Not in the
// anonymous namespace below (unlike the RegisterXFn typedefs): it names a
// member type of PluginLoader itself, declared at minimal::plugins scope in
// the header.
struct ProjectCommandsPlugin {
    void* handle;
    std::string name;
    PluginRegistrationContext context;
};

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
using RegisterProjectCommandsFn = void (*)(commands::CommandRegistry&,
                                           const std::filesystem::path&,
                                           const PluginRegistrationContext&);
using ProjectCommandsFingerprintFn = void (*)(const std::filesystem::path&, std::string&);
using RegisterAuthProviderFn =
    void (*)(rpc::AuthProviderRegistry&, const PluginRegistrationContext&);
using RegisterMentionResolverFn =
    void (*)(agent::MentionResolverRegistry&, const PluginRegistrationContext&);
using RegisterContextContributorsFn =
    void (*)(agent::ContextContributorRegistry&, const PluginRegistrationContext&);
using RegisterPermissionAdvisorFn =
    void (*)(tools::PermissionAdvisorRegistry&, const PluginRegistrationContext&);
using RegisterModelCatalogsFn =
    void (*)(llm::ModelCatalogRegistry&, const PluginRegistrationContext&);
using RegisterMetadataCatalogsFn =
    void (*)(llm::MetadataCatalogRegistry&, const PluginRegistrationContext&);
using RegisterVariantParamsFn =
    void (*)(llm::VariantParamsRegistry&, const PluginRegistrationContext&);
using RegisterCompactionStrategiesFn =
    void (*)(agent::CompactionStrategyRegistry&, const PluginRegistrationContext&);
using RegisterServicesFn =
    void (*)(ServiceRegistry&, const PluginRegistrationContext&);
using RegisterHookPointsFn =
    void (*)(hooks::HookDispatcher&, const PluginRegistrationContext&);

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
// closing it and reporting on either exit. On a throw, `rollback` runs after
// the window closes: it strips what the plugin managed to register before
// failing (see LoadOne), because a user plugin's failure is only a warning and
// partially-initialized registrations would otherwise stay served.
//
// PLUGIN CODE IS NOT TRUSTED TO RETURN NORMALLY. These callbacks were called
// bare: a throw propagated out of LoadOne, out of LoadAll, into main() -- which
// has no handler -- so the process died with no diagnostic naming the plugin,
// and the attribution window stayed open, mis-attributing every registration
// after it to a plugin that had already failed. Every OTHER plugin entry point
// in this tree is wrapped (observers in Dispatch, commands in Dispatch, tools in
// ToolExecutor::Run); registration was the one that was not.
template <typename Open, typename Body, typename Close, typename Rollback>
bool GuardedRegistration(PluginRecord& record, const char* what, Open&& open, Body&& body,
                          Close&& close, Rollback&& rollback) {
    open();
    try {
        body();
    } catch (const std::exception& error) {
        close();
        rollback();
        record.status = LoadStatus::RegistrationThrew;
        record.error = std::string(what) + " threw: " + error.what();
        return false;
    } catch (...) {
        close();
        rollback();
        record.status = LoadStatus::RegistrationThrew;
        record.error = std::string(what) + " threw a non-standard exception";
        return false;
    }
    close();
    return true;
}

// The data dir is keyed by the plugin's own declared name
// (minimal_plugin_name), which is plugin-controlled input: "../..", an
// absolute name, or anything carrying a separator would place the provisioned
// directory outside plugin_data_root (or replace it outright). A name is one
// plain directory component, nothing more. No other validation exists for
// record.name -- the NameRegistry checks collisions and dangling aliases,
// not name shape.
bool IsSafeDataDirName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos;
}

}  // namespace

// One registry the loader drives: the ABI symbol that fills it, the label
// PluginRecord::registered files its entries under, and the uniform
// attribution/rollback surface every registry now offers.
struct PluginLoader::Target {
    Target(const char* symbol_in, const char* label_in) : symbol(symbol_in), label(label_in) {}
    virtual ~Target() = default;
    // Runs the plugin's hook if it exports one.
    virtual void Invoke(void* handle, const PluginRegistrationContext& context) = 0;
    virtual void Open(const std::string& plugin_name) = 0;
    virtual void Close() = 0;
    virtual void Remove(const std::string& plugin_name) = 0;
    // What `plugin_name` holds in this registry, for the startup listing.
    virtual std::vector<std::string> Owned(const std::string& plugin_name) const = 0;

    const char* symbol;
    const char* label;
};

namespace {

template <typename Registry>
class TargetFor final : public PluginLoader::Target {
public:
    using OwnedFn = std::function<std::vector<std::string>(const Registry&, const std::string&)>;

    TargetFor(const char* symbol, const char* label, Registry& registry, OwnedFn owned)
        : Target(symbol, label), registry_(registry), owned_(std::move(owned)) {}

    void Invoke(void* handle, const PluginRegistrationContext& context) override {
        using Fn = void (*)(Registry&, const PluginRegistrationContext&);
        if (auto fn = Lookup<Fn>(handle, symbol)) fn(registry_, context);
    }
    void Open(const std::string& plugin_name) override { registry_.SetAttribution(plugin_name); }
    void Close() override { registry_.ClearAttribution(); }
    void Remove(const std::string& plugin_name) override {
        registry_.RemoveByAttribution(plugin_name);
    }
    std::vector<std::string> Owned(const std::string& plugin_name) const override {
        return owned_(registry_, plugin_name);
    }

private:
    Registry& registry_;
    OwnedFn owned_;
};

template <typename Registry>
void AddTarget(std::vector<std::unique_ptr<PluginLoader::Target>>& targets, const char* symbol,
               const char* label, Registry* registry,
               typename TargetFor<Registry>::OwnedFn owned) {
    if (registry == nullptr) return;
    targets.push_back(
        std::make_unique<TargetFor<Registry>>(symbol, label, *registry, std::move(owned)));
}

// Owned() for registries keyed by kind.
template <typename Registry>
std::vector<std::string> KindsOwned(const Registry& registry, const std::string& plugin) {
    return registry.KeysOwnedBy(plugin);
}

// Owned() for registries whose entries carry no name of their own: one
// `description` per entry the plugin holds.
std::vector<std::string> Repeat(std::size_t count, const char* description) {
    return std::vector<std::string>(count, description);
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

PluginLoader::PluginLoader(HostRegistries registries, nlohmann::json tools_settings,
                           PluginStorage* storage, std::filesystem::path plugin_data_root)
    : registries_(registries),
      tools_settings_(std::move(tools_settings)),
      storage_(storage),
      plugin_data_root_(std::move(plugin_data_root)) {
    BuildTargets();
}

void PluginLoader::BuildTargets() {
    AddTarget(targets_, "minimal_plugin_register_hook_points", "hook", registries_.hooks,
              [](const hooks::HookDispatcher& registry, const std::string& plugin) {
                  std::vector<std::string> out;
                  for (const auto& entry : registry.attributions()) {
                      if (entry.plugin_name == plugin) out.push_back(entry.point_name);
                  }
                  return out;
              });
    AddTarget(targets_, "minimal_plugin_register_tools", "tool", registries_.tools,
              [](const tools::ToolRegistry& registry, const std::string& plugin) {
                  std::vector<std::string> out;
                  for (const auto& entry : registry.attributions()) {
                      if (entry.plugin_name == plugin) out.push_back(entry.tool_name);
                  }
                  return out;
              });
    AddTarget(targets_, "minimal_plugin_register_provider_factories", "provider", registries_.providers,
              KindsOwned<llm::ProviderFactoryRegistry>);
    AddTarget(targets_, "minimal_plugin_register_commands", "command", registries_.commands,
              [](const commands::CommandRegistry& registry, const std::string& plugin) {
                  std::vector<std::string> out;
                  for (const auto& entry : registry.attributions()) {
                      if (entry.plugin_name == plugin) out.push_back(entry.surface_name);
                  }
                  return out;
              });
    AddTarget(targets_, "minimal_plugin_register_session_observer", "observer", registries_.observers,
              [](const agent::SessionObserverRegistry& registry, const std::string& plugin) {
                  return Repeat(registry.CountOwnedBy(plugin), "session lifecycle");
              });
    AddTarget(targets_, "minimal_plugin_register_auth_provider", "auth", registries_.auth_providers,
              [](const rpc::AuthProviderRegistry& registry, const std::string& plugin) {
                  return Repeat(registry.OwnedBy(plugin) ? 1 : 0, "connection policy");
              });
    AddTarget(targets_, "minimal_plugin_register_context_contributors", "context",
              registries_.context_contributors,
              [](const agent::ContextContributorRegistry& registry, const std::string& plugin) {
                  return Repeat(registry.CountOwnedBy(plugin), "session prefill");
              });
    AddTarget(targets_, "minimal_plugin_register_mention_resolver", "mention", registries_.mention_resolvers,
              [](const agent::MentionResolverRegistry& registry, const std::string& plugin) {
                  return Repeat(registry.OwnedBy(plugin) ? 1 : 0, "@-mention resolver");
              });
    AddTarget(targets_, "minimal_plugin_register_permission_advisor", "advisor",
              registries_.permission_advisors,
              [](const tools::PermissionAdvisorRegistry& registry, const std::string& plugin) {
                  return Repeat(registry.Has(plugin) ? 1 : 0, "permission prompts");
              });
    AddTarget(targets_, "minimal_plugin_register_permission_models", "permission model",
              registries_.permission_models, KindsOwned<tools::PermissionModelRegistry>);
    AddTarget(targets_, "minimal_plugin_register_tool_sources", "tool source",
              registries_.tool_sources, KindsOwned<tools::ToolSourceRegistry>);
    AddTarget(targets_, "minimal_plugin_register_model_catalogs", "catalog", registries_.model_catalogs,
              KindsOwned<llm::ModelCatalogRegistry>);
    AddTarget(targets_, "minimal_plugin_register_metadata_catalogs", "metadata",
              registries_.metadata_catalogs,
              [](const llm::MetadataCatalogRegistry& registry, const std::string& plugin) {
                  std::vector<std::string> out = registry.KeysOwnedBy(plugin);
                  if (registry.HasFallbackFrom(plugin)) out.push_back(kMetadataFallbackLabel);
                  return out;
              });
    AddTarget(targets_, "minimal_plugin_register_variant_params", "variant", registries_.variant_params,
              KindsOwned<llm::VariantParamsRegistry>);
    AddTarget(targets_, "minimal_plugin_register_compaction_strategies", "compaction",
              registries_.compaction_strategies, KindsOwned<agent::CompactionStrategyRegistry>);
    AddTarget(targets_, "minimal_plugin_register_services", "service", registries_.services,
              [](const ServiceRegistry& registry, const std::string& plugin) {
                  std::vector<std::string> out;
                  for (const auto& entry : registry.attributions()) {
                      if (entry.plugin_name == plugin) out.push_back(entry.service_name);
                  }
                  return out;
              });
}

// Handles are intentionally leaked -- see LIFETIME in the header.
PluginLoader::~PluginLoader() = default;

void PluginLoader::ShutdownAll() {
    if (shut_down_) return;
    shut_down_ = true;
    using ShutdownFn = void (*)();
    for (auto it = loaded_.rbegin(); it != loaded_.rend(); ++it) {
        auto fn = Lookup<ShutdownFn>(it->second, "minimal_plugin_shutdown");
        if (fn == nullptr) continue;
        try {
            fn();
        } catch (const std::exception& e) {
            common::DefaultLogger().warn("plugin '{}': shutdown threw: {}", it->first, e.what());
        } catch (...) {
            common::DefaultLogger().warn("plugin '{}': shutdown threw", it->first);
        }
    }
}

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

    // ABI tag is verified BEFORE any other plugin code runs. The old order
    // called name_fn()/version_fn() alongside the tag fetch -- both are
    // plugin-executed functions, and running them from a .so built against a
    // different toolchain/ABI is exactly what the ABI gate exists to prevent.
    // A mismatch report therefore names the FILE, not the plugin's own name
    // (which we no longer read).
    if (const char* tag = abi_tag_fn(); tag != nullptr) record.abi_tag = tag;
    // Checked before any registration callback runs: a plugin built against a
    // different toolchain must never get to execute against our types.
    if (record.abi_tag != std::string(common::AbiTag())) {
        record.status = LoadStatus::AbiMismatch;
        record.error = "built for '" + record.abi_tag + "', host is '" +
                        std::string(common::AbiTag()) + "' -- rebuild the plugin";
        return record;
    }

    // Safe now: the ABI matched, so these run against compatible types.
    if (const char* name = name_fn(); name != nullptr) record.name = name;
    if (const char* version = version_fn(); version != nullptr) record.version = version;

    // Everything the registration hooks see: the shared settings.json `tools`
    // object plus THIS plugin's own plugins.json `config` slice. Built once per
    // plugin, before any hook runs.
    PluginRegistrationContext context;
    context.tools_settings = tools_settings_;
    context.config = candidate.config;
    // The consumer side of the service channel: the same registry that
    // register_services below dispatches into, so a plugin capturing this
    // pointer at registration resolves services from this process's registry.
    context.services = registries_.services;
    // Per-plugin storage and its own data dir. The dir is provisioned HERE,
    // keyed by the plugin's declared name, because only the host can guarantee
    // it exists before the hooks that will use it run.
    context.storage = storage_;
    context.report_warning = [this, name = record.name](const std::string& message) {
        warnings_[name].push_back(message);
    };
    if (!plugin_data_root_.empty() && !record.name.empty()) {
        if (!IsSafeDataDirName(record.name)) {
            // Skip provisioning, keep loading: the plugin simply runs with
            // context.data_dir unset rather than writing outside the root.
            common::DefaultLogger().warn(
                "plugin '{}': refusing to provision a data dir for an unsafe name",
                record.name);
        } else {
            std::error_code ec;
            const auto dir = plugin_data_root_ / record.name;
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                common::DefaultLogger().warn("plugin '{}': could not create data dir {}: {}",
                                             record.name, dir.string(), ec.message());
            } else {
                context.data_dir = dir;
            }
        }
    }

    // A user plugin's failure is only a warning, so a plugin whose hook throws
    // must not leave live, attributed registrations behind: everything it
    // registered, in every registry, is removed.
    auto rollback = [this, &record] {
        for (const auto& target : targets_) target->Remove(record.name);
        common::DefaultLogger().warn(
            "plugin '{}': registration threw; everything it registered was removed", record.name);
    };

    // Each hook runs inside its registry's attribution window, which is what
    // lets a collision name both plugins and a rollback find what to remove.
    // Order matters once: hook points go first, so a tool can capture the
    // HookPoint handle its own plugin just registered.
    for (const auto& target : targets_) {
        if (!GuardedRegistration(
                record, target->symbol, [&] { target->Open(record.name); },
                [&] { target->Invoke(handle, context); }, [&] { target->Close(); }, rollback)) {
            return record;
        }
    }
    for (const auto& target : targets_) {
        std::vector<std::string> owned = target->Owned(record.name);
        if (!owned.empty()) record.registered[target->label] = std::move(owned);
    }
    if (const auto it = warnings_.find(record.name); it != warnings_.end()) {
        record.warnings = it->second;
    }

    // The OPTIONAL per-root commands hook is never CALLED here (there is no
    // project root at startup) -- only remembered, with a copy of this
    // plugin's `context`, for RegisterProjectCommands to call later, per root.
    if (registries_.commands != nullptr &&
        Lookup<RegisterProjectCommandsFn>(handle, "minimal_plugin_register_project_commands") !=
            nullptr) {
        project_command_plugins_.push_back(
            std::make_unique<ProjectCommandsPlugin>(ProjectCommandsPlugin{handle, record.name, context}));
    }

    // Loaded only after the LAST hook completed: the sanity check skips
    // Loaded records, so a failure must never be reported as one.
    record.status = LoadStatus::Loaded;
    loaded_.emplace_back(record.name, handle);

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

void PluginLoader::RegisterProjectCommands(commands::CommandRegistry& registry,
                                           const std::filesystem::path& project_root) const {
    for (const auto& plugin_ptr : project_command_plugins_) {
        const ProjectCommandsPlugin& plugin = *plugin_ptr;
        auto fn = Lookup<RegisterProjectCommandsFn>(plugin.handle,
                                                     "minimal_plugin_register_project_commands");
        if (fn == nullptr) continue;   // checked at LoadOne time; defensive
        // Same attribution window register_commands uses, for the same
        // reason: a collision within this project registry should be able to
        // name which plugin is responsible.
        registry.SetAttribution(plugin.name);
        try {
            fn(registry, project_root, plugin.context);
        } catch (const std::exception& error) {
            common::DefaultLogger().warn(
                "plugin '{}': minimal_plugin_register_project_commands threw for project root "
                "'{}': {}",
                plugin.name, project_root.string(), error.what());
        } catch (...) {
            common::DefaultLogger().warn(
                "plugin '{}': minimal_plugin_register_project_commands threw a non-standard "
                "exception for project root '{}'",
                plugin.name, project_root.string());
        }
        registry.ClearAttribution();
    }
}

std::string PluginLoader::ProjectCommandsFingerprint(
    const std::filesystem::path& project_root) const {
    std::string out;
    for (const auto& plugin_ptr : project_command_plugins_) {
        const ProjectCommandsPlugin& plugin = *plugin_ptr;
        auto fn = Lookup<ProjectCommandsFingerprintFn>(
            plugin.handle, "minimal_plugin_project_commands_fingerprint");
        if (fn == nullptr) continue;
        // The ABI says "must not throw"; a plugin that does anyway costs a
        // rebuild on every lookup (its value reads as changed), never a
        // failed lookup.
        std::string value;
        try {
            fn(project_root, value);
        } catch (...) {
            value = "<threw>";
        }
        out += plugin.name;
        out += '\0';
        out += value;
        out += '\0';
    }
    return out;
}

}  // namespace minimal::plugins
