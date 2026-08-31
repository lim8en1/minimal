#pragma once

// The CLIENT plugin ABI. This is the header a client plugin author includes.
// The server's twin is src/server/plugins/include/minimal/plugins/plugin_abi.h;
// the same conventions apply in full.
//
// COMPATIBILITY MODEL: source, not binary. Plugins are compiled from the same
// tree with the same pinned toolchain as the harness, so an interface change is
// a compile error you fix by rebuilding -- not a silent break. The ABI tag is
// therefore a "you forgot to rebuild" guard, not a stability promise.
//
// The header has two halves, and both are frozen surface:
//
//   1. EXTENSION POINTS a plugin implements -- the extern "C" symbols below.
//      Symbol presence *is* the capability declaration; the loader dlsym's each
//      optional name and calls it only if found, so there is no manifest to
//      drift out of sync with the code.
//
//   2. UTILITIES a plugin consumes -- the interfaces in registries.hpp
//      (AttentionSink, ClientApi) plus the render types.
//
// EVERY registration symbol takes (Registry&, const ClientPluginRegistrationContext&)
// from the first one shipped. The context is a struct so a later addition stays
// source-compatible instead of forcing another signature sweep on every plugin
// author -- the lesson the server learned with PluginRegistrationContext (the
// 2.3 generalization re-swept every symbol once; this header starts where that
// landed).

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/common/abi_tag.hpp"

namespace minimal::client::plugins {

// Everything a plugin needs at REGISTRATION time. A struct, per the rule above.
struct ClientPluginRegistrationContext {
    // This plugin's own configuration, verbatim: the optional "config" object
    // on its client-plugins.json entry. Core imposes no schema; the same rule
    // as the server's per-plugin config. Empty when the entry carries none.
    nlohmann::json config = nlohmann::json::object();

    // The attention sink (registries.hpp): raise/clear tab or global attention.
    // Null when the host does not offer the seam (a test exercising other
    // registries), which a plugin treats as "no attention", not an error.
    AttentionSink* attention = nullptr;

    // The sanctioned data path (EXTENSIBILITY 4.2): scrollback pages through
    // the same RPC core uses. Null on the same terms as `attention`.
    ClientApi* client_api = nullptr;
};

}  // namespace minimal::client::plugins

extern "C" {

// REQUIRED. A plugin missing any of these is skipped, not loaded.
//
// Implement abi_tag by returning minimal::common::AbiTag() -- returning a
// literal defeats the guard, since the point is to report the toolchain this
// binary was actually built with.
const char* minimal_client_plugin_abi_tag();
const char* minimal_client_plugin_name();
const char* minimal_client_plugin_version();

// OPTIONAL, additive: self-contained views (panel or modal).
void minimal_client_plugin_register_views(
    minimal::client::plugins::ViewRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: renderers for one tool's result, keyed by tool name. A
// renderer for a CORE tool (a diff view for `edit`) is as legitimate as one
// for a plugin tool; a renderer for a tool whose plugin is disabled falls back
// to the default rendering rather than erroring.
void minimal_client_plugin_register_renderers(
    minimal::client::plugins::ToolResultRendererRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: message-kind presentation, keyed by ChatRowKind. Core
// supplies the defaults for every kind a plugin does not claim.
void minimal_client_plugin_register_message_renderers(
    minimal::client::plugins::MessageRendererRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: status-bar segments, keyed by segment id; the format
// string orders and selects.
void minimal_client_plugin_register_status_segments(
    minimal::client::plugins::StatusSegmentRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: syntax-highlight grammars, keyed by language; the
// vendored set registers as the defaults. Two plugins claiming one language is
// fatal at startup, like every other name collision.
void minimal_client_plugin_register_syntax_grammars(
    minimal::client::plugins::SyntaxGrammarRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, EXCLUSIVE: replace the whole chat view. One plugin; a second
// registration warns and is ignored.
void minimal_client_plugin_register_chat_surface(
    minimal::client::plugins::ChatSurfaceRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, EXCLUSIVE: replace the markdown engine (the md4c adapter is the
// core default). The replacement carries the streaming contract itself --
// partial input cannot soundly parse; Finalize() always reparses
// authoritatively (EXTENSIBILITY 4.2).
void minimal_client_plugin_register_markdown_engine(
    minimal::client::plugins::MarkdownEngineRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

}  // extern "C"
