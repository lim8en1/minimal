#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/plugin_config.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/plugins/name_registry.hpp"

namespace minimal::client::plugins {

// "Came out of this build" vs. "someone dropped this in" -- the same trust
// distinction the server loader encodes, driving default enablement and the
// severity of a load failure. The client surfaces it as startup diagnostics:
// a failed BUNDLED plugin is fatal (the TUI prints and exits -- a silent
// missing renderer would read as a bug), a failed user plugin is a warning.
// Shared with the server host (src/common/plugins/plugin_config.cpp).
using Provenance = common::PluginProvenance;

enum class LoadStatus {
    Loaded,
    Disabled,               // client-plugins.json says enabled:false; never opened
    OpenFailed,             // dlopen failed (missing file, bad arch, unresolved symbol)
    MissingRequiredSymbol,  // not a minimal client plugin, or built against a different ABI header
    AbiMismatch,            // built against a different toolchain; rebuild it
    RegistrationThrew,      // a registration callback threw (see the server loader's history)
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

    // ORIGINAL keys, as the plugin declared them, by RegistryHook::label
    // ("views", "tool_renderers", ...) -- collision detection and diagnostics
    // are computed from "who provided what". An exclusive slot is listed
    // only when this plugin holds it.
    std::map<std::string, std::vector<std::string>> registered;

    bool ok() const { return status == LoadStatus::Loaded; }
};

std::string_view ToString(LoadStatus status);

// The host implementations a plugin's registration context carries.
struct PluginHostSeams {
    AttentionSink* attention = nullptr;
    ClientApi* client_api = nullptr;
    FocusProvider* focus = nullptr;
    HostUi* ui = nullptr;
};

// One row per registry, the single table both the loader (hook lookup,
// attribution window, rollback, PluginRecord::registered) and the bootstrap
// (collision feed, alias targets, seal) walk. A new registry is one row in
// RegistryHooks. Rows are in hook-call order.
struct RegistryHook {
    std::string_view label;   // PluginRecord::registered's key
    const char* symbol;       // the optional minimal_client_plugin_register_* export
    RegistryBase* registry;
    // The NameRegistry kind its keys collide under; nullopt where keys never
    // collide (an exclusive slot's rule is its own: first wins, the rest warn).
    std::optional<minimal::plugins::NameKind> name_kind;
    // A client-plugins.json alias may rename its entries.
    bool aliasable = false;
    // Calls the looked-up hook with the registry at its real type.
    void (*invoke)(void* hook, RegistryBase& registry,
                   const ClientPluginRegistrationContext& context) = nullptr;
};

std::vector<RegistryHook> RegistryHooks(ClientRegistries& registries);

// Opens client plugins and invokes whichever registration seams they export.
// Mirrors the server loader exactly (RTLD_NOW | RTLD_LOCAL, required tag/name/
// version trio checked BEFORE any callback, guarded registration, attribution
// windows) -- one rule, two loaders, which is why NameRegistry itself moved to
// common.
//
// LIFETIME: handles are held until the process exits and are deliberately
// never dlclose'd. A registered View's vtable lives inside its .so; unloading
// while the registry holds its objects is a use-after-free that surfaces as an
// unrelated crash much later. Do not "clean this up".
class ClientPluginLoader {
public:
    // The host owns `registries` and outlives the loader. `seams` are the
    // host implementations handed to plugins in the context; any may be
    // null (a test exercising only registration), meaning the seam is simply
    // not offered.
    ClientPluginLoader(ClientRegistries& registries, PluginHostSeams seams = {});
    ~ClientPluginLoader();

    ClientPluginLoader(const ClientPluginLoader&) = delete;
    ClientPluginLoader& operator=(const ClientPluginLoader&) = delete;

    // Loads every enabled candidate. One plugin's failure never aborts the
    // others: every outcome, good or bad, comes back as a PluginRecord.
    std::vector<PluginRecord> LoadAll(const std::vector<PluginCandidate>& candidates);

private:
    PluginRecord LoadOne(const PluginCandidate& candidate);

    // Names of the plugins this loader has LOADED. Attribution -- and so a
    // failed hook's rollback, RemoveByAttribution(name) -- is keyed by name:
    // a second library reporting a loaded plugin's name (both copies of a
    // "replaced" bundled plugin enabled) could strip the healthy one's
    // registrations when its own hook threw. Refused before any hook runs.
    std::set<std::string> loaded_names_;

    std::vector<RegistryHook> hooks_;
    PluginHostSeams seams_;
    std::vector<void*> handles_;  // never closed; see LIFETIME above
};

}  // namespace minimal::client::plugins
