#include "minimal/client/plugins/client_plugin_loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "minimal/client/plugins/client_plugin_abi.h"
#include "minimal/common/abi_tag.hpp"
#include "minimal/common/logger.hpp"

namespace minimal::client::plugins {
namespace {

using AbiTagFn = const char* (*)();
using NameFn = const char* (*)();
using VersionFn = const char* (*)();
using RegisterViewsFn =
    void (*)(ViewRegistry&, const ClientPluginRegistrationContext&);
using RegisterStartViewsFn =
    void (*)(StartViewRegistry&, const ClientPluginRegistrationContext&);
using RegisterRenderersFn =
    void (*)(ToolResultRendererRegistry&, const ClientPluginRegistrationContext&);
using RegisterDisplayRenderersFn =
    void (*)(DisplayRendererRegistry&, const ClientPluginRegistrationContext&);
using RegisterMessageRenderersFn =
    void (*)(MessageRendererRegistry&, const ClientPluginRegistrationContext&);
using RegisterStatusSegmentsFn =
    void (*)(StatusSegmentRegistry&, const ClientPluginRegistrationContext&);
using RegisterSyntaxGrammarsFn =
    void (*)(SyntaxGrammarRegistry&, const ClientPluginRegistrationContext&);
using RegisterChatSurfaceFn =
    void (*)(ChatSurfaceRegistry&, const ClientPluginRegistrationContext&);
using RegisterMarkdownEngineFn =
    void (*)(MarkdownEngineRegistry&, const ClientPluginRegistrationContext&);

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
    }
    return "unknown";
}

ClientPluginLoader::ClientPluginLoader(ViewRegistry& views,
                                       ToolResultRendererRegistry& renderers,
                                       DisplayRendererRegistry& display_renderers,
                                       MessageRendererRegistry& message_renderers,
                                       StatusSegmentRegistry& status_segments,
                                       SyntaxGrammarRegistry& grammars,
                                       ChatSurfaceRegistry& chat_surface,
                                       MarkdownEngineRegistry& markdown_engine,
                                       StartViewRegistry& start_views, AttentionSink* attention,
                                       ClientApi* client_api, FocusProvider* focus)
    : views_(views),
      renderers_(renderers),
      display_renderers_(display_renderers),
      message_renderers_(message_renderers),
      status_segments_(status_segments),
      grammars_(grammars),
      chat_surface_(chat_surface),
      markdown_engine_(markdown_engine),
      start_views_(start_views),
      attention_(attention),
      client_api_(client_api),
      focus_(focus) {}

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

    // Everything the registration hooks see: this plugin's own config slice,
    // plus the host-owned seams the context carries. Built once per plugin,
    // before any hook runs.
    ClientPluginRegistrationContext context;
    context.config = candidate.config;
    context.attention = attention_;
    context.client_api = client_api_;
    context.focus = focus_;

    // Partial-registration rollback, captured BEFORE the first hook runs
    // (ported from the server loader, plugin_loader.cpp:233-260). Every
    // client registry has a removal seam -- unlike the server's partial
    // coverage, there is no "stays live" caveat to log here.
    auto rollback = [this, &record] {
        views_.RemoveByAttribution(record.name);
        start_views_.RemoveByAttribution(record.name);
        renderers_.RemoveByAttribution(record.name);
        display_renderers_.RemoveByAttribution(record.name);
        message_renderers_.RemoveByAttribution(record.name);
        status_segments_.RemoveByAttribution(record.name);
        grammars_.RemoveByAttribution(record.name);
        chat_surface_.RemoveByAttribution(record.name);
        markdown_engine_.RemoveByAttribution(record.name);
        common::DefaultLogger().warn(
            "client plugin '{}': registration threw; rolled back -- nothing it registered "
            "stayed live",
            record.name);
    };

    if (auto register_views = Lookup<RegisterViewsFn>(handle, "minimal_client_plugin_register_views")) {
        const std::size_t before = views_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_views",
                [&] { views_.SetAttribution(record.name); },
                [&] { register_views(views_, context); },
                [&] { views_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < views_.attributions().size(); ++i) {
            record.registered_views.push_back(views_.attributions()[i].original_name);
        }
    }

    if (auto register_start_views = Lookup<RegisterStartViewsFn>(
            handle, "minimal_client_plugin_register_start_views")) {
        const std::size_t before = start_views_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_start_views",
                [&] { start_views_.SetAttribution(record.name); },
                [&] { register_start_views(start_views_, context); },
                [&] { start_views_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < start_views_.attributions().size(); ++i) {
            record.registered_start_views.push_back(
                start_views_.attributions()[i].original_name);
        }
    }

    if (auto register_renderers =
            Lookup<RegisterRenderersFn>(handle, "minimal_client_plugin_register_renderers")) {
        const std::size_t before = renderers_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_renderers",
                [&] { renderers_.SetAttribution(record.name); },
                [&] { register_renderers(renderers_, context); },
                [&] { renderers_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < renderers_.attributions().size(); ++i) {
            record.registered_renderers.push_back(renderers_.attributions()[i].original_name);
        }
    }

    if (auto register_display_renderers = Lookup<RegisterDisplayRenderersFn>(
            handle, "minimal_client_plugin_register_display_renderers")) {
        const std::size_t before = display_renderers_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_display_renderers",
                [&] { display_renderers_.SetAttribution(record.name); },
                [&] { register_display_renderers(display_renderers_, context); },
                [&] { display_renderers_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < display_renderers_.attributions().size(); ++i) {
            record.registered_display_kinds.push_back(
                display_renderers_.attributions()[i].original_name);
        }
    }

    if (auto register_message_renderers = Lookup<RegisterMessageRenderersFn>(
            handle, "minimal_client_plugin_register_message_renderers")) {
        const std::size_t before = message_renderers_.claims().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_message_renderers",
                [&] { message_renderers_.SetAttribution(record.name); },
                [&] { register_message_renderers(message_renderers_, context); },
                [&] { message_renderers_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < message_renderers_.claims().size(); ++i) {
            record.registered_message_kinds.push_back(message_renderers_.claims()[i].second.original_name);
        }
    }

    if (auto register_status_segments = Lookup<RegisterStatusSegmentsFn>(
            handle, "minimal_client_plugin_register_status_segments")) {
        const std::size_t before = status_segments_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_status_segments",
                [&] { status_segments_.SetAttribution(record.name); },
                [&] { register_status_segments(status_segments_, context); },
                [&] { status_segments_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < status_segments_.attributions().size(); ++i) {
            record.registered_status_segments.push_back(
                status_segments_.attributions()[i].original_name);
        }
    }

    if (auto register_grammars = Lookup<RegisterSyntaxGrammarsFn>(
            handle, "minimal_client_plugin_register_syntax_grammars")) {
        const std::size_t before = grammars_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_syntax_grammars",
                [&] { grammars_.SetAttribution(record.name); },
                [&] { register_grammars(grammars_, context); },
                [&] { grammars_.ClearAttribution(); }, rollback)) {
            return record;
        }
        for (std::size_t i = before; i < grammars_.attributions().size(); ++i) {
            record.registered_grammars.push_back(grammars_.attributions()[i].original_name);
        }
    }

    if (auto register_chat_surface = Lookup<RegisterChatSurfaceFn>(
            handle, "minimal_client_plugin_register_chat_surface")) {
        if (!GuardedRegistration(record, "minimal_client_plugin_register_chat_surface",
                [&] { chat_surface_.SetAttribution(record.name); },
                [&] { register_chat_surface(chat_surface_, context); },
                [&] { chat_surface_.ClearAttribution(); }, rollback)) {
            return record;
        }
        record.registered_chat_surface = chat_surface_.record.registered &&
                                         chat_surface_.record.plugin_name == record.name;
    }

    if (auto register_markdown_engine = Lookup<RegisterMarkdownEngineFn>(
            handle, "minimal_client_plugin_register_markdown_engine")) {
        if (!GuardedRegistration(record, "minimal_client_plugin_register_markdown_engine",
                [&] { markdown_engine_.SetAttribution(record.name); },
                [&] { register_markdown_engine(markdown_engine_, context); },
                [&] { markdown_engine_.ClearAttribution(); }, rollback)) {
            return record;
        }
        record.registered_markdown_engine = markdown_engine_.record.registered &&
                                            markdown_engine_.record.plugin_name == record.name;
    }

    record.status = LoadStatus::Loaded;
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
