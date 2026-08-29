#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/llm/provider_factory_registry.hpp"
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
};

struct PluginRecord {
    std::filesystem::path file;
    std::string name;
    std::string version;
    std::string abi_tag;
    Provenance provenance = Provenance::User;
    LoadStatus status = LoadStatus::OpenFailed;
    std::string error;  // human-readable; empty when Loaded or Disabled

    std::vector<std::string> registered_tools;
    std::vector<std::string> registered_provider_kinds;

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
    PluginLoader(tools::ToolRegistry& tools, llm::ProviderFactoryRegistry& providers,
                 nlohmann::json tools_settings = nlohmann::json::object());
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
    std::vector<void*> handles_;  // never closed; see LIFETIME above
};

}  // namespace minimal::plugins
