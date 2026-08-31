#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/session_observer_registry.hpp"
#include "minimal/commands/command_registry.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/rpc/auth_provider.hpp"
#include "minimal/tools/permission_advisor.hpp"
#include "minimal/tools/tool_registry.hpp"

namespace minimal::plugins {

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
    // True when the plugin registered an advisor that actually landed (a
    // duplicate from the same plugin is refused and does not count twice).
    bool registered_permission_advisor = false;

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
                 tools::PermissionAdvisorRegistry* permission_advisors = nullptr);
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
    // Optional for the same reason observers_ is: a test exercising only tool
    // registration has no command surface to offer, and building one would be
    // scaffolding for nothing.
    commands::CommandRegistry* commands_ = nullptr;
    // Optional like the others; null means plugins cannot register an auth
    // provider. Exclusive registry: the first plugin to register wins.
    rpc::AuthProviderRegistry* auth_providers_ = nullptr;
    // Optional like the others; null means plugins cannot register a permission
    // advisor. Additive across plugins; permissions.json selects one by name.
    tools::PermissionAdvisorRegistry* permission_advisors_ = nullptr;
    std::vector<void*> handles_;  // never closed; see LIFETIME above
};

}  // namespace minimal::plugins
