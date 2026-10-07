#include "minimal/client/plugins/client_plugin_loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "minimal/client/sdk/client_plugin_abi.h"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {
namespace {

using AbiTagFn = const char* (*)();
using NameFn = const char* (*)();
using VersionFn = const char* (*)();
// A hook's call, with the registry at its real type. The dlsym address is
// converted the way Lookup does (see there).
template <typename Registry>
void InvokeHook(void* hook, RegistryBase& registry, const ClientPluginRegistrationContext& context) {
    using Fn = void (*)(Registry&, const ClientPluginRegistrationContext&);
    Fn fn = nullptr;
    static_assert(sizeof(fn) == sizeof(hook), "function/object pointer size mismatch");
    std::memcpy(&fn, &hook, sizeof(fn));
    fn(static_cast<Registry&>(registry), context);
}

template <typename Registry>
RegistryHook Hook(std::string_view label, const char* symbol, Registry& registry,
                  std::string_view name_space = {},
                  minimal::plugins::NameTable::Policy policy = minimal::plugins::NameTable::Policy::kSlot,
                  bool required = false) {
    return {.label = label,
            .symbol = symbol,
            .registry = &registry,
            .name_space = name_space,
            .policy = policy,
            .required = required,
            .invoke = &InvokeHook<Registry>};
}

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

// PLUGIN CODE IS NOT TRUSTED TO RETURN NORMALLY. Same story as the server
// loader, which learned it the hard way: a throw out of a bare callback died
// in main() with no diagnostic and left the attribution window open. Every
// registration below runs inside this guard.
// The server loader's three-lambda shape (track R4): the attribution window's
// CLOSE runs on both exit paths. The first client draft closed it inside the
// body lambda -- on a throwing plugin the window stayed open, and every later
// registration was mis-attributed to the plugin that had already failed.
//
// `rollback` runs after `close`, on a throw only (ported from the server
// loader, plugin_loader.cpp:54-88): without it, whatever this plugin
// registered in an EARLIER hook this same LoadOne call stayed live even
// though the plugin as a whole failed to load -- a later, healthy plugin
// reusing one of those ids got a fatal collision naming a plugin that never
// actually loaded.
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

}  // namespace

std::string_view ToString(LoadStatus status) {
    switch (status) {
        case LoadStatus::Loaded:                return "loaded";
        case LoadStatus::Disabled:              return "disabled";
        case LoadStatus::OpenFailed:            return "open failed";
        case LoadStatus::MissingRequiredSymbol: return "missing required symbol";
        case LoadStatus::AbiMismatch:           return "abi mismatch";
        case LoadStatus::RegistrationThrew:     return "registration threw";
        case LoadStatus::DuplicateName:         return "duplicate plugin name";
    }
    return "unknown";
}

std::vector<RegistryHook> RegistryHooks(ClientRegistries& r) {
    using Policy = minimal::plugins::NameTable::Policy;
    return {
        // kName: user-addressed names (keybindings.json, ui.json status_format).
        // kSlot: the key is a contract with something outside the plugin (a
        // payload kind, a tool name, a language; client commands dispatch by
        // their original name). Start views share the view-id namespace.
        Hook("views", "minimal_client_plugin_register_views", r.views, "view id", Policy::kName),
        Hook("start_views", "minimal_client_plugin_register_start_views", r.start_views,
             "view id", Policy::kName),
        Hook("client_commands", "minimal_client_plugin_register_client_commands",
             r.client_commands, "client command", Policy::kSlot),
        Hook("tool_renderers", "minimal_client_plugin_register_renderers", r.tool_renderers,
             "tool result renderer", Policy::kSlot),
        Hook("display_renderers", "minimal_client_plugin_register_display_renderers",
             r.display_renderers, "display renderer", Policy::kSlot),
        Hook("prompt_renderers", "minimal_client_plugin_register_prompt_renderers",
             r.prompt_renderers, "prompt renderer", Policy::kSlot),
        Hook("message_renderers", "minimal_client_plugin_register_message_renderers",
             r.message_renderers, "message renderer", Policy::kSlot),
        Hook("status_segments", "minimal_client_plugin_register_status_segments",
             r.status_segments, "status segment", Policy::kName),
        Hook("grammars", "minimal_client_plugin_register_syntax_grammars", r.grammars,
             "syntax grammar", Policy::kSlot),
        // No key to collide on (one constant per observer).
        Hook("observers", "minimal_client_plugin_register_observers", r.observers),
        // An exclusive slot's key is the slot: a second claimant collides.
        // Required: the chat is the TUI's body.
        Hook("chat_surface", "minimal_client_plugin_register_chat_surface", r.chat_surface,
             "exclusive slot", Policy::kSlot, /*required=*/true),
        Hook("markdown_engine", "minimal_client_plugin_register_markdown_engine",
             r.markdown_engine, "exclusive slot", Policy::kSlot),
        // Required: every view draws with it.
        Hook("widget_kit", "minimal_client_plugin_register_widget_kit", r.widget_kit,
             "exclusive slot", Policy::kSlot, /*required=*/true),
        // Required: the only way the TUI takes input.
        Hook("prompt_editor", "minimal_client_plugin_register_prompt_editor", r.prompt_editor,
             "exclusive slot", Policy::kSlot, /*required=*/true),
        Hook("tab_bar", "minimal_client_plugin_register_tab_bar", r.tab_bar, "exclusive slot",
             Policy::kSlot),
    };
}

ClientPluginLoader::ClientPluginLoader(ClientRegistries& registries, PluginHostSeams seams)
    : hooks_(RegistryHooks(registries)), seams_(seams) {}

// Handles are intentionally leaked -- see LIFETIME in the header.
ClientPluginLoader::~ClientPluginLoader() = default;

PluginRecord ClientPluginLoader::LoadOne(const PluginCandidate& candidate) {
    PluginRecord record;
    record.file = candidate.file;
    record.provenance = candidate.provenance;

    if (!candidate.enabled) {
        record.status = LoadStatus::Disabled;
        return record;
    }

    // RTLD_NOW so an unresolved symbol surfaces here, with a name attached,
    // rather than as a crash the first time that code path runs.
    // RTLD_LOCAL keeps this plugin's symbols out of the global namespace; it
    // does not prevent resolving *host* symbols, which is what plugins need.
    void* handle = dlopen(candidate.file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        record.status = LoadStatus::OpenFailed;
        const char* message = dlerror();
        record.error = message ? message : "dlopen failed";
        return record;
    }
    handles_.push_back(handle);

    auto abi_tag_fn = Lookup<AbiTagFn>(handle, "minimal_client_plugin_abi_tag");
    auto name_fn = Lookup<NameFn>(handle, "minimal_client_plugin_name");
    auto version_fn = Lookup<VersionFn>(handle, "minimal_client_plugin_version");
    if (abi_tag_fn == nullptr || name_fn == nullptr || version_fn == nullptr) {
        record.status = LoadStatus::MissingRequiredSymbol;
        record.error =
            "not a minimal client plugin: missing minimal_client_plugin_abi_tag/name/version";
        return record;
    }

    // ABI tag is verified BEFORE any other plugin code runs (the server loader
    // fixed exactly this ordering): name_fn()/version_fn() are
    // plugin-executed functions, and running them from a .so built against a
    // different toolchain/ABI is what the ABI gate exists to prevent. A
    // mismatch report therefore names the FILE's tag only -- the plugin's own
    // name is not read yet.
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

    // A null/empty name would leave record.name == "", the same sentinel core
    // registrations use for their own attributions (see tui_bootstrap.cpp's
    // RegisterCoreSyntaxGrammars, registered before any plugin loads). If this
    // plugin then registered something and a later hook threw, rollback's
    // RemoveByAttribution("") would strip every ""-attributed entry, including
    // core's -- so a malformed plugin name is rejected outright, never
    // silently coerced to the shared sentinel.
    if (record.name.empty()) {
        record.status = LoadStatus::MissingRequiredSymbol;
        record.error = "minimal_client_plugin_name() returned null or an empty string";
        return record;
    }
    if (loaded_names_.count(record.name) != 0) {
        record.status = LoadStatus::DuplicateName;
        record.error = "a plugin named '" + record.name +
                       "' is already loaded -- disable one of the two copies";
        return record;
    }

    // A client-plugins.d file that exists but cannot be read fails the plugin
    // (the server loader's rule): running it with {} would silently drop
    // every setting the user wrote.
    if (candidate.config_error) {
        record.status = LoadStatus::RegistrationThrew;
        record.error = *candidate.config_error;
        return record;
    }

    // Everything the registration hooks see: this plugin's own
    // client-plugins.d file, plus the host-owned seams the context carries.
    // Built once per plugin, before any hook runs.
    ClientPluginRegistrationContext context;
    // Always an object: "no config" is {}, never null, so every plugin parser
    // sees one shape.
    context.config = candidate.config.is_null() ? nlohmann::json::object() : candidate.config;
    context.attention = seams_.attention;
    context.client_api = seams_.client_api;
    context.focus = seams_.focus;
    context.widgets = seams_.widgets;
    context.actions = seams_.actions;
    context.highlighter = seams_.highlighter;
    context.markdown = seams_.markdown;
    context.ui = seams_.ui_for_plugin ? seams_.ui_for_plugin(record.name) : seams_.ui;

    // Partial-registration rollback, captured BEFORE the first hook runs
    // (ported from the server loader, plugin_loader.cpp:233-260). Every
    // client registry has a removal seam -- unlike the server's partial
    // coverage, there is no "stays live" caveat to log here.
    auto rollback = [this, &record] {
        for (const RegistryHook& hook : hooks_) hook.registry->RemoveByAttribution(record.name);
        record.registered.clear();
        common::DefaultLogger().warn(
            "client plugin '{}': registration threw; rolled back -- nothing it registered "
            "stayed live",
            record.name);
    };

    for (const RegistryHook& hook : hooks_) {
        void* fn = dlsym(handle, hook.symbol);
        if (fn == nullptr) continue;   // every register_* hook is optional
        RegistryBase& registry = *hook.registry;
        const std::size_t before = registry.attributions().size();
        if (!GuardedRegistration(record, hook.symbol,
                [&] { registry.SetAttribution(record.name); },
                [&] { hook.invoke(fn, registry, context); },
                [&] { registry.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < registry.attributions().size(); ++i) {
            record.registered[std::string(hook.label)].push_back(
                registry.attributions()[i].original_name);
        }
    }

    record.status = LoadStatus::Loaded;
    loaded_names_.insert(record.name);
    return record;
}

std::vector<PluginRecord> ClientPluginLoader::LoadAll(
    const std::vector<PluginCandidate>& candidates) {
    std::vector<PluginRecord> records;
    records.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        records.push_back(LoadOne(candidate));
    }
    return records;
}

}  // namespace minimal::client::plugins
