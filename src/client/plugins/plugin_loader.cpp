#include "minimal/client/plugins/client_plugin_loader.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "minimal/common/abi_tag.hpp"
#include "minimal/client/plugins/client_plugin_abi.h"

namespace minimal::client::plugins {
namespace {

using AbiTagFn = const char* (*)();
using NameFn = const char* (*)();
using VersionFn = const char* (*)();
using RegisterViewsFn =
    void (*)(ViewRegistry&, const ClientPluginRegistrationContext&);
using RegisterRenderersFn =
    void (*)(ToolResultRendererRegistry&, const ClientPluginRegistrationContext&);
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
template <typename Body>
bool GuardedRegistration(PluginRecord& record, const char* what, Body&& body) {
    try {
        body();
    } catch (const std::exception& error) {
        record.status = LoadStatus::RegistrationThrew;
        record.error = std::string(what) + " threw: " + error.what();
        return false;
    } catch (...) {
        record.status = LoadStatus::RegistrationThrew;
        record.error = std::string(what) + " threw a non-standard exception";
        return false;
    }
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
                                       MessageRendererRegistry& message_renderers,
                                       StatusSegmentRegistry& status_segments,
                                       SyntaxGrammarRegistry& grammars,
                                       ChatSurfaceRegistry& chat_surface,
                                       MarkdownEngineRegistry& markdown_engine,
                                       AttentionSink* attention, ClientApi* client_api)
    : views_(views),
      renderers_(renderers),
      message_renderers_(message_renderers),
      status_segments_(status_segments),
      grammars_(grammars),
      chat_surface_(chat_surface),
      markdown_engine_(markdown_engine),
      attention_(attention),
      client_api_(client_api) {}

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

    // Everything the registration hooks see: this plugin's own config slice,
    // plus the host-owned seams the context carries. Built once per plugin,
    // before any hook runs.
    ClientPluginRegistrationContext context;
    context.config = candidate.config;
    context.attention = attention_;
    context.client_api = client_api_;

    if (auto register_views = Lookup<RegisterViewsFn>(handle, "minimal_client_plugin_register_views")) {
        const std::size_t before = views_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_views", [&] {
                views_.SetAttribution(record.name);
                register_views(views_, context);
                views_.ClearAttribution();
            })) {
            return record;
        }
        for (std::size_t i = before; i < views_.attributions().size(); ++i) {
            record.registered_views.push_back(views_.attributions()[i].original_name);
        }
    }

    if (auto register_renderers =
            Lookup<RegisterRenderersFn>(handle, "minimal_client_plugin_register_renderers")) {
        const std::size_t before = renderers_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_renderers", [&] {
                renderers_.SetAttribution(record.name);
                register_renderers(renderers_, context);
                renderers_.ClearAttribution();
            })) {
            return record;
        }
        for (std::size_t i = before; i < renderers_.attributions().size(); ++i) {
            record.registered_renderers.push_back(renderers_.attributions()[i].original_name);
        }
    }

    if (auto register_message_renderers = Lookup<RegisterMessageRenderersFn>(
            handle, "minimal_client_plugin_register_message_renderers")) {
        const std::size_t before = message_renderers_.claims().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_message_renderers", [&] {
                message_renderers_.SetAttribution(record.name);
                register_message_renderers(message_renderers_, context);
                message_renderers_.ClearAttribution();
            })) {
            return record;
        }
        for (std::size_t i = before; i < message_renderers_.claims().size(); ++i) {
            record.registered_message_kinds.push_back(message_renderers_.claims()[i].second.original_name);
        }
    }

    if (auto register_status_segments = Lookup<RegisterStatusSegmentsFn>(
            handle, "minimal_client_plugin_register_status_segments")) {
        const std::size_t before = status_segments_.attributions().size();
        if (!GuardedRegistration(record, "minimal_client_plugin_register_status_segments", [&] {
                status_segments_.SetAttribution(record.name);
                register_status_segments(status_segments_, context);
                status_segments_.ClearAttribution();
            })) {
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
        if (!GuardedRegistration(record, "minimal_client_plugin_register_syntax_grammars", [&] {
                grammars_.SetAttribution(record.name);
                register_grammars(grammars_, context);
                grammars_.ClearAttribution();
            })) {
            return record;
        }
        for (std::size_t i = before; i < grammars_.attributions().size(); ++i) {
            record.registered_grammars.push_back(grammars_.attributions()[i].original_name);
        }
    }

    if (auto register_chat_surface = Lookup<RegisterChatSurfaceFn>(
            handle, "minimal_client_plugin_register_chat_surface")) {
        if (!GuardedRegistration(record, "minimal_client_plugin_register_chat_surface", [&] {
                chat_surface_.SetAttribution(record.name);
                register_chat_surface(chat_surface_, context);
                chat_surface_.ClearAttribution();
            })) {
            return record;
        }
        record.registered_chat_surface = chat_surface_.record.registered &&
                                         chat_surface_.record.plugin_name == record.name;
    }

    if (auto register_markdown_engine = Lookup<RegisterMarkdownEngineFn>(
            handle, "minimal_client_plugin_register_markdown_engine")) {
        if (!GuardedRegistration(record, "minimal_client_plugin_register_markdown_engine", [&] {
                markdown_engine_.SetAttribution(record.name);
                register_markdown_engine(markdown_engine_, context);
                markdown_engine_.ClearAttribution();
            })) {
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
