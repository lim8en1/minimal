#pragma once

// The TUI's startup sequence, factored out of RunTui (app.cpp): ui.json load
// -> vendored grammar registration -> client plugin discovery/load/collision
// resolution -> theme load -> keybindings load/keymap parse -> status format
// parse -> syntax highlighter/markdown renderer/engine wiring -> chat
// surface wiring. ARCHITECTURE.md's "config + client plugins" ORDER MATTERS
// comment (app.cpp) states the exact dependency chain; this file performs it
// verbatim, just no longer inline in a 1000+ line function.
//
// *** ADDRESS-STABILITY CONTRACT -- READ BEFORE TOUCHING THIS FILE ***
// Several fields below hold a pointer or reference into ANOTHER field of the
// SAME TuiEnvironment: `attention_sink` captures `&attention_state`,
// `highlighter` captures `theme` by reference AND captures `&languages`
// (SyntaxHighlighter stores `const Theme&` and, in the grammar-table
// constructor, a `const std::vector<LanguageDefinition>*`), `renderer`
// captures `&*theme` and `&*highlighter` (MarkdownRenderer stores raw
// pointers), and `engine` is a reference into `engine_registry`'s own
// heap-owned MarkdownEngine. None of this is safe under a MOVE: if the whole
// struct were ever returned by value or otherwise relocated, every one of
// those captured addresses would go stale -- the exact "closures/pointers
// outlive the object they point into" bug class documented throughout
// app.cpp (see e.g. MarkdownRenderer's own construction comment).
// `languages` in particular MUST live here, not as a local in
// BuildTuiEnvironment: a local's storage ends when the function returns,
// which is exactly what `highlighter` is still pointing at afterwards --
// this was a real bug (a dangling `languages_` deref, SIGSEGV on the first
// rendered fenced code block, in every session thereafter since the
// transcript persists the block and resuming re-renders it).
// BuildTuiEnvironment therefore takes `out` BY REFERENCE and fills it IN
// PLACE rather than returning a TuiEnvironment by value: the caller must
// declare `TuiEnvironment env;` as a plain local and never move, copy, or
// return it. `theme`/`highlighter`/`renderer` are std::optional specifically
// because SyntaxHighlighter/MarkdownRenderer are not default-constructible
// (reference/pointer members) -- emplace() constructs them IN the optional's
// own inline storage, which lives at a fixed offset within `out` for as long
// as `out` itself is never relocated.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/keybindings.hpp"
#include "minimal/client/plugins/host_seams.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/syntax_highlighter.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/status_format.hpp"
#include "minimal/client/ui_config.hpp"
#include "minimal/plugins/name_registry.hpp"

namespace minimal::client {

// plugins::PluginViewEntry (registries.hpp) is the runtime dispatch entry
// type; it lives there rather than here so keybinding_dispatch.hpp -- which
// dispatches global keys to visible plugin views -- can share it without
// including this header (which itself includes keybinding_dispatch.hpp for
// ResolvedKeymap; a cycle either way round).
using plugins::PluginViewEntry;

struct TuiEnvironment {
    std::filesystem::path config_dir;
    ColorDepth depth = ColorDepth::Truecolor;
    UiConfig ui;

    plugins::AttentionState attention_state;
    std::unique_ptr<plugins::AttentionSink> attention_sink;
    std::unique_ptr<plugins::ClientApi> host_client_api;
    // Concrete type: app.cpp attaches the (later-constructed) AppState after
    // BuildTuiEnvironment returns -- see host_seams.hpp for why the seam's
    // target cannot be supplied at construction time.
    std::unique_ptr<plugins::HostFocusProvider> host_focus_provider;

    plugins::ViewRegistry view_registry;
    plugins::StartViewRegistry start_view_registry;
    plugins::ToolResultRendererRegistry renderer_registry;
    plugins::MessageRendererRegistry message_registry;
    plugins::StatusSegmentRegistry status_registry;
    plugins::SyntaxGrammarRegistry grammar_registry;
    plugins::ChatSurfaceRegistry surface_registry;
    plugins::MarkdownEngineRegistry engine_registry;

    // The raw action->key-STRING table (core defaults + the user's file +
    // every plugin view's EnsureDefault), kept alongside `keymap` below for
    // the keymap help overlay -- ResolvedKeymap only stores parsed Event
    // candidates, not the original spec strings, so it cannot answer "what
    // key is bound to this action" as human-readable text.
    Keybindings keybindings;
    ResolvedKeymap keymap;
    std::vector<StatusSegment> status_format;

    // The assembled grammar table (vendored defaults plus plugin additions):
    // `highlighter` below stores a POINTER into this vector, so it must live
    // here rather than as a BuildTuiEnvironment local -- see the
    // address-stability contract above. Declared before `highlighter` so
    // destruction runs in the safe order too.
    std::vector<LanguageDefinition> languages;

    // See the address-stability contract above: constructed via emplace(),
    // never reassigned wholesale.
    std::optional<Theme> theme;
    std::optional<SyntaxHighlighter> highlighter;
    std::optional<MarkdownRenderer> renderer;

    // Non-owning: `engine` points at engine_registry's own heap-owned
    // default/plugin engine; `chat_surface` at surface_registry's exclusive
    // surface (null if none claimed the slot, or if one threw on OnTheme).
    plugins::MarkdownEngine* engine = nullptr;
    plugins::ChatSurface* chat_surface = nullptr;

    std::vector<PluginViewEntry> plugin_views;
};

// Runs the whole startup sequence into `out`, which the caller must have
// declared as a plain local (see the address-stability contract above) --
// never a temporary, never later moved or returned by value. Diagnostics
// (plugin load warnings, unknown status segments, unmapped keybindings, a
// malformed config file) print to stderr exactly as RunTui did inline.
// Returns false on a FATAL condition (a bundled plugin failed to load, or a
// name collision among client plugins) -- the caller's contract is to treat
// that as minimal's kUnavailable exit code, same as the inline code's
// `return kUnavailable` did at each of these points.
bool BuildTuiEnvironment(SessionClient& client, TuiEnvironment& out);

// One kind's attribution list, for MatchAliasKind below. `attributions`
// points into a registry's own storage (stable for BuildTuiEnvironment's
// whole call), never owned here.
struct AliasCandidate {
    minimal::plugins::NameKind kind;
    const std::vector<plugins::Attribution>* attributions;
};

// The alias-resolution matching decision BuildTuiEnvironment applies to
// every entry in client-plugins.json's "aliases" map: which NameKind a
// qualified "plugin.original_name" alias key belongs to. Checks `candidates`
// first (view/tool-renderer/status-segment/syntax-grammar registries, in
// whatever order the caller supplies -- BuildTuiEnvironment includes BOTH
// view_registry and start_view_registry under NameKind::ViewId, since both
// register views addressable the same way), then `message_claims`
// separately (MessageRendererRegistry::claims() is (ChatRowKind,
// Attribution) pairs, a different shape from the others' plain
// attributions()). Falls back to NameKind::ViewId -- the "dangling" case,
// since ViewId is the canonical kind keybindings.json addresses -- when no
// candidate matches at all.
//
// Pure and independently testable: takes only Attribution/ChatRowKind data,
// no registries, no plugin loading required to exercise it.
minimal::plugins::NameKind MatchAliasKind(
    const std::vector<AliasCandidate>& candidates,
    const std::vector<std::pair<ChatRowKind, plugins::Attribution>>& message_claims,
    const std::string& key);

}  // namespace minimal::client
