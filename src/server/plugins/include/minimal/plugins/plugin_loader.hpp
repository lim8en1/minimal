#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/plugin_config.hpp"
#include "minimal/agent/compaction_strategy.hpp"
#include "minimal/agent/context_contributor.hpp"
#include "minimal/agent/mention_resolver.hpp"
#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/commands/project_command_source.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/agent/host_registries.hpp"
#include "minimal/llm/metadata_catalog.hpp"
#include "minimal/llm/model_catalog.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/llm/variant_params.hpp"
#include "minimal/plugins/service_registry.hpp"
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
// The "metadata" entry for a plugin that appended a fallback enrichment
// catalog (one that answers for kinds nobody named).
inline constexpr const char* kMetadataFallbackLabel = "(fallback: kinds nobody named)";

// Shared with the client host (src/common/plugins/plugin_config.cpp).
using Provenance = common::PluginProvenance;

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
    DuplicateName,          // an already-loaded plugin reported the same name
};

using PluginCandidate = common::PluginFileCandidate;

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
    // What this plugin holds, per registry label ("tool", "provider",
    // "command", "observer", ...; see PluginLoader::BuildTargets). Kind- and
    // name-keyed registries list their keys (commands by surface name, after
    // aliasing); registries whose entries carry no name list one description
    // per entry. Absent label: the plugin registered nothing there.
    std::map<std::string, std::vector<std::string>> registered;

    // Configuration problems the plugin reported through
    // PluginRegistrationContext::report_warning during registration.
    std::vector<std::string> warnings;

    const std::vector<std::string>& Registered(std::string_view label) const {
        static const std::vector<std::string> kNone;
        const auto it = registered.find(std::string(label));
        return it == registered.end() ? kNone : it->second;
    }

    bool ok() const { return status == LoadStatus::Loaded; }
};

std::string_view ToString(LoadStatus status);

// Opens plugins and invokes whichever registration seams they export.
//
// LIFETIME: handles are held until the process exits and are deliberately never
// dlclose'd. A registered Tool's vtable lives inside its .so, so unloading a
// plugin while the registry still holds its objects is a use-after-free that
// surfaces as an unrelated crash much later. Do not "clean this up".
class PluginLoader : public commands::ProjectCommandProvider {
public:
    // `registries`: every seam a plugin may register into; a null member is
    // simply not offered (plugins treat every registration hook as optional).
    // `tools_settings` is the `tools` object from settings.json, forwarded
    // verbatim to every plugin; core imposes no schema on it. `storage` (may be
    // null) is per-plugin KV storage, forwarded through the registration
    // context. `plugin_data_root` is where each plugin's own data_dir is
    // provisioned (<root>/<plugin_name>/); empty disables provisioning.
    explicit PluginLoader(HostRegistries registries,
                          nlohmann::json tools_settings = nlohmann::json::object(),
                          PluginStorage* storage = nullptr,
                          std::filesystem::path plugin_data_root = {});
    ~PluginLoader();

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Loads every enabled candidate. One plugin's failure never aborts the
    // others: every outcome, good or bad, comes back as a PluginRecord.
    std::vector<PluginRecord> LoadAll(const std::vector<PluginCandidate>& candidates);

    // Calls every LOADED plugin's optional minimal_plugin_shutdown, in reverse
    // load order, containing throws. Idempotent; main.cpp calls it once, after
    // the sessions are terminated.
    void ShutdownAll();

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
                                 const std::filesystem::path& project_root) const override;

    // Every project-commands plugin's OPTIONAL
    // minimal_plugin_project_commands_fingerprint for `project_root`, joined
    // with the plugin names. ProjectCommandSource rebuilds a root's registry
    // when this changes. Empty when no plugin exports the hook.
    std::string ProjectCommandsFingerprint(
        const std::filesystem::path& project_root) const override;

    // One registry the loader drives; defined in plugin_loader.cpp.
    struct Target;

private:
    PluginRecord LoadOne(const PluginCandidate& candidate);
    void BuildTargets();

    HostRegistries registries_;
    nlohmann::json tools_settings_;
    PluginStorage* storage_ = nullptr;
    std::filesystem::path plugin_data_root_;
    std::vector<std::unique_ptr<Target>> targets_;  // registration order
    std::vector<void*> handles_;  // never closed; see LIFETIME above
    // The Loaded subset, in load order, with names: ShutdownAll's list.
    std::vector<std::pair<std::string, void*>> loaded_;
    bool shut_down_ = false;
    // report_warning's sink, per plugin name: the loader outlives every
    // context copy (project-command contexts are kept past LoadOne).
    std::map<std::string, std::vector<std::string>> warnings_;

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
