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

#include "minimal/client/sdk/registries.hpp"
#include "minimal/common/abi_tag.hpp"

namespace minimal::client::plugins {

// Everything a plugin needs at REGISTRATION time. A struct, per the rule above.
struct ClientPluginRegistrationContext {
    // This plugin's own configuration, verbatim: the object in its own file,
    // <config dir>/client-plugins.d/<stem>.json. Core imposes no schema; the
    // same rule as the server's per-plugin config. Empty when the file does
    // not exist.
    nlohmann::json config = nlohmann::json::object();

    // The attention sink (registries.hpp): raise/clear tab or global attention.
    // Null when the host does not offer the seam (a test exercising other
    // registries), which a plugin treats as "no attention", not an error.
    AttentionSink* attention = nullptr;

    // The sanctioned data path (EXTENSIBILITY 4.2): scrollback pages through
    // the same RPC core uses. Null on the same terms as `attention`.
    ClientApi* client_api = nullptr;

    // Which tab/session the user is looking at (registries.hpp FocusProvider):
    // the route a session-aware panel view takes besides the status segments'
    // per-render argument. Null on the same terms as `attention` -- a plugin
    // degrades to focus-blind content, never an error.
    FocusProvider* focus = nullptr;

    // Ask the host to repaint (registries.hpp HostUi) after the plugin's own
    // data changed off the UI thread. Null on the same terms as `attention`.
    HostUi* ui = nullptr;

    // The widget kit (registries.hpp WidgetKit) every view builds on --
    // whichever implementation holds the slot, resolved per call, so it may
    // be used from component() and later even when the kit's own plugin
    // loads after this one. Not during registration: the slot is filled
    // after every plugin has registered. Null on the same terms as `ui`.
    WidgetKit* widgets = nullptr;

    // What the plugin asks the TUI to do (registries.hpp HostActions):
    // queued from any thread, applied on the UI thread. Null on the same
    // terms as `ui`.
    HostActions* actions = nullptr;

    // The host's code highlighter (render/syntax_highlighter.hpp), over every
    // registered grammar -- what a markdown engine colors fenced code with.
    // Usable from the first render (it is built after the plugins load).
    // Null on the same terms as `ui`.
    const CodeHighlighter* highlighter = nullptr;

    // The markdown engine (the markdown_engine slot's holder, or core's plain
    // one): a proxy resolving the slot per call, so it works whichever
    // plugin loads first. Usable once registration is over (from render).
    const MarkdownEngine* markdown = nullptr;
};

}  // namespace minimal::client::plugins

extern "C" {

// REQUIRED. A plugin missing any of these is skipped, not loaded.
//
// Implement these with MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS below, not by hand --
// see the server twin's identical comment (plugin_abi.h) for why a handwritten
// `return minimal::common::AbiTag();` is silently wrong: minimal_common is
// STATIC, linked PUBLIC into the SHARED minimal_client_core, so a plugin's
// undefined reference to AbiTag() binds at dlopen time to the running host's
// copy, not to anything baked into the plugin. The macro instead expands the
// compile-time MINIMAL_ABI_TAG/MINIMAL_VERSION macros, which the compiler
// bakes into the plugin's own object code.
const char* minimal_client_plugin_abi_tag();
const char* minimal_client_plugin_name();
const char* minimal_client_plugin_version();

// OPTIONAL, EXCLUSIVE: the tab bar (registries.hpp TabBar) -- the bundled
// tab-bar plugin's, or a replacement. None: no bar.
void minimal_client_plugin_register_tab_bar(
    minimal::client::plugins::TabBarRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

}  // extern "C"

// Defines the three REQUIRED symbols above. Every client plugin should use
// this instead of writing them by hand. Invoke once, at file scope, in the
// plugin's .cpp (the three declarations above already gave these C linkage,
// so no `extern "C"` is needed at the call site).
#define MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS(plugin_name_literal)                          \
    const char* minimal_client_plugin_abi_tag() { return MINIMAL_ABI_TAG; }             \
    const char* minimal_client_plugin_name() { return (plugin_name_literal); }          \
    const char* minimal_client_plugin_version() { return MINIMAL_VERSION; }

extern "C" {

// OPTIONAL, additive: self-contained views (panel or modal).
void minimal_client_plugin_register_views(
    minimal::client::plugins::ViewRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: the initial view (the empty state and a fresh session
// before its first message). Many plugins may contribute; all render, stacked
// in registration order. A duplicate id is fatal through the name resolution,
// like every other view id.
void minimal_client_plugin_register_start_views(
    minimal::client::plugins::StartViewRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: client-side /-commands (registries.hpp ClientCommand),
// run in the TUI without a server round trip. The client's own commands win a
// name collision (the plugin's is ignored with a warning).
void minimal_client_plugin_register_client_commands(
    minimal::client::plugins::ClientCommandRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: renderers for one tool's result, keyed by tool name. A
// renderer for a CORE tool (a diff view for `edit`) is as legitimate as one
// for a plugin tool; a renderer for a tool whose plugin is disabled falls back
// to the default rendering rather than erroring.
void minimal_client_plugin_register_renderers(
    minimal::client::plugins::ToolResultRendererRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: renderers for one KIND of tool-result display payload
// ("diff", "file", ...), for every tool that emits it. Core fills the core
// kinds no plugin claims; two plugins claiming one kind is fatal at startup.
void minimal_client_plugin_register_display_renderers(
    minimal::client::plugins::DisplayRendererRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, additive: modal renderers for one KIND of plugin prompt (the
// client half of a server tool calling ToolContext::prompt_user), keyed by
// kind. Two plugins claiming one kind is fatal at startup. A prompt whose kind
// no plugin claims is answered UNAVAILABLE by the host.
void minimal_client_plugin_register_prompt_renderers(
    minimal::client::plugins::PromptRendererRegistry&,
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

// OPTIONAL, additive: observers of host events (tab opened/closed, focus,
// turn start/end, session metadata), delivered on the UI thread.
void minimal_client_plugin_register_observers(
    minimal::client::plugins::ObserverRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// REQUIRED, EXCLUSIVE: the chat view (registries.hpp ChatSurface) -- the
// bundled chat-view plugin's, or a replacement. Two claimants are fatal.
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

// OPTIONAL, EXCLUSIVE: replace the widget kit -- the menus, form and modal
// chrome every view builds on (registries.hpp WidgetKit; core's own is the
// default).
void minimal_client_plugin_register_widget_kit(
    minimal::client::plugins::WidgetKitRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// REQUIRED, EXCLUSIVE: the prompt editor (registries.hpp PromptEditorFactory)
// -- the bundled prompt-editor plugin's, or a replacement.
void minimal_client_plugin_register_prompt_editor(
    minimal::client::plugins::PromptEditorRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

// OPTIONAL, EXCLUSIVE: the tab bar (registries.hpp TabBar) -- the bundled
// tab-bar plugin's, or a replacement. None: no bar.
void minimal_client_plugin_register_tab_bar(
    minimal::client::plugins::TabBarRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&);

}  // extern "C"
