#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/registries.hpp"

namespace minimal::client::plugins {

class ClientApi;          // declared in client_plugin_abi.h; host-implemented
class FocusProvider;      // declared in registries.hpp; host-implemented

// "Came out of this build" vs. "someone dropped this in" -- the same trust
// distinction the server loader encodes, driving default enablement and the
// severity of a load failure. The client surfaces it as startup diagnostics:
// a failed BUNDLED plugin is fatal (the TUI prints and exits -- a silent
// missing renderer would read as a bug), a failed user plugin is a warning.
enum class Provenance { Bundled, User };

enum class LoadStatus {
    Loaded,
    Disabled,               // client-plugins.json says enabled:false; never opened
    OpenFailed,             // dlopen failed (missing file, bad arch, unresolved symbol)
    MissingRequiredSymbol,  // not a minimal client plugin, or built against a different ABI header
    AbiMismatch,            // built against a different toolchain; rebuild it
    RegistrationThrew,      // a registration callback threw (see the server loader's history)
    DuplicateName,          // an already-loaded plugin reported the same name
};

struct PluginCandidate {
    std::filesystem::path file;
    Provenance provenance = Provenance::User;
    bool enabled = false;
    // The candidate's own config from its client-plugins.json entry, verbatim.
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

    // ORIGINAL keys, as the plugin declared them -- collision detection and
    // diagnostics are computed from "who provided what".
    std::vector<std::string> registered_views;
    std::vector<std::string> registered_start_views;
    std::vector<std::string> registered_renderers;    // tool names
    std::vector<std::string> registered_display_kinds;
    std::vector<std::string> registered_prompt_kinds;
    std::vector<std::string> registered_status_segments;
    std::vector<std::string> registered_grammars;     // language keys
    std::vector<std::string> registered_message_kinds;
    bool registered_chat_surface = false;
    bool registered_markdown_engine = false;

    bool ok() const { return status == LoadStatus::Loaded; }
};

std::string_view ToString(LoadStatus status);

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
    // Registries are references -- the host owns them and outlives the loader.
    // `attention`, `client_api` and `focus` are the host implementations handed
    // to plugins in the context; null is allowed (a test exercising only
    // registration) and means the seam is simply not offered.
    ClientPluginLoader(ViewRegistry& views, ToolResultRendererRegistry& renderers,
                       DisplayRendererRegistry& display_renderers,
                       MessageRendererRegistry& message_renderers,
                       StatusSegmentRegistry& status_segments,
                       SyntaxGrammarRegistry& grammars, ChatSurfaceRegistry& chat_surface,
                       MarkdownEngineRegistry& markdown_engine,
                       StartViewRegistry& start_views, AttentionSink* attention,
                       ClientApi* client_api, FocusProvider* focus,
                       PromptRendererRegistry* prompt_renderers = nullptr);
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

    ViewRegistry& views_;
    ToolResultRendererRegistry& renderers_;
    DisplayRendererRegistry& display_renderers_;
    MessageRendererRegistry& message_renderers_;
    StatusSegmentRegistry& status_segments_;
    SyntaxGrammarRegistry& grammars_;
    ChatSurfaceRegistry& chat_surface_;
    MarkdownEngineRegistry& markdown_engine_;
    StartViewRegistry& start_views_;
    // Null when the host offers no prompt slot (a test exercising other
    // registries): a plugin's prompt renderers are then simply not loaded.
    PromptRendererRegistry* prompt_renderers_ = nullptr;
    AttentionSink* attention_ = nullptr;
    ClientApi* client_api_ = nullptr;
    FocusProvider* focus_ = nullptr;
    std::vector<void*> handles_;  // never closed; see LIFETIME above
};

}  // namespace minimal::client::plugins
