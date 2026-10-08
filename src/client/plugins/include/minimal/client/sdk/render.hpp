#pragma once

// The render seams: row renderers (by message kind, tool name or display
// kind), status segments, syntax grammars, the markdown engine and the tab
// bar. Render-time inputs travel with the call -- including `const Theme&`:
// plugins load BEFORE the theme does (theme `sections` need to know which
// plugins are loaded), so nothing renderable can capture a theme at
// registration. Every Render runs on the UI thread: cheap, never blocking.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/sdk/chat_model.hpp"
#include "minimal/client/sdk/highlighter.hpp"
#include "minimal/client/sdk/host.hpp"
#include "minimal/client/sdk/policy.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::client::plugins {

// The highlighter's rule type IS the ABI type -- no translation between a
// registered grammar and core.
using SyntaxRule = minimal::client::SyntaxRule;

using LanguageFnPtr = minimal::client::LanguageFnPtr;

// --- Tab bar (EXCLUSIVE, OPTIONAL) -----------------------------------------

// What a tab filter (tab.filter_group / tab.filter_agents) keeps out of the
// bar. A filter hides identity, so hidden ATTENTION has to surface here or a
// filtered-out prompt would be invisible.
struct TabBarBadge {
    bool active = false;
    std::string label;          // "group" | "agents"; "" while no filter is on
    std::size_t hidden = 0;     // tabs the filter keeps out of the bar
    std::size_t attention = 0;  // of those, carrying attention
};

// The bar's input, computed by core each frame: the VISIBLE tabs in order
// (the filter applied -- the filter is a core verb), without tool rows, and
// the badge.
struct TabBarModel {
    std::vector<TabActivity> tabs;
    TabBarBadge badge;
};

// Where each drawn tab cell landed, for click-to-focus: the bar reports a box
// per cell (ftxui::reflect into it), core hit-tests the next click.
struct TabBarHits {
    std::vector<std::pair<std::string, std::shared_ptr<ftxui::Box>>> cells;
    std::string TabAt(int x, int y) const {
        for (const auto& [tab_id, box] : cells) {
            if (x >= box->x_min && x <= box->x_max && y >= box->y_min && y <= box->y_max) {
                return tab_id;
            }
        }
        return {};
    }
};

// The row of tabs above the chat. Optional: with none registered there is no
// bar. Render runs every frame, on the render thread: cheap, no blocking. A
// null Element draws no row (the bundled bar hides itself for a lone tab).
// Obligation: a tab with `attention` shows a color-independent mark (the
// bundled bar's '!') -- the one signal that survives any theme.
class TabBar {
public:
    virtual ~TabBar() = default;
    virtual ftxui::Element Render(const TabBarModel& model, const Theme& theme,
                                  TabBarHits& hits) const = 0;
};

// --- Row render context (collapse state) ------------------------------------
//
// Travels with every MessageRenderer/ToolResultRenderer call, same as `theme`
// -- the "render-time inputs travel with the call" rule above. `collapsed` is
// the row's RESOLVED state (the three-tier per-row/global/kind-default
// resolution lives in ChatView, not here): a renderer decides what collapsed
// vs. expanded LOOKS like for its own kind, never whether a row is collapsed.
struct RowRenderContext {
    bool collapsed = false;
    // The CURRENT binding for chat.toggle_collapse, composed by the host
    // (ChatView) from the resolved keymap. Renderers have no keymap access,
    // so hints used to hardcode "alt+c" -- which lied the moment the user
    // remapped the action.
    std::string toggle_collapse_hint = "alt+c";
    // The click affordance for collapsible rows: a disclosure glyph, "▸"
    // while collapsed, "▾" while expanded, "" for kinds a click never
    // collapses. Core renderers draw it LEADING the row's header line; a
    // renderer for a collapsible kind that ignores it simply forgoes the
    // visual hint (clicking still works).
    std::string_view collapse_marker = "";
    // AgentMarkdown only: an earlier assistant text row of the SAME turn (and
    // the same agent/model) already carried the speaker label, so this one
    // should not repeat it -- a tool-calling turn used to say "assistant" once
    // per text segment. See LabelContinuations (chat_model.hpp).
    bool continuation = false;
    // The row is part of the turn that is still streaming: its text may be a
    // partial prefix, and it renders again as it grows (every frame while it
    // changes -- keep it cheap). Only a PLUGIN-claimed kind is asked to draw
    // live rows (Thinking, ToolActivity); core keeps its own incremental live
    // rendering for the kinds it owns.
    bool live = false;
};

// Everything a tool-result renderer draws FROM. One struct rather than a
// parameter list, so a new field is not a signature sweep across plugins.
struct ToolResultView {
    std::string_view tool_name;
    // The JSON the model sent with the call; empty object when unavailable.
    const nlohmann::json& tool_input;
    // The tool's display payload ({"kind": ..., ...}); null when it sent none.
    const nlohmann::json& display;
    // What the model saw -- the universal fallback.
    std::string_view result_text;
    bool is_error = false;
};

// --- Tool-result renderers (keyed by tool name) -----------------------------

// A renderer for one tool's RESULT row, for a tool and renderer shipped as a
// pair. Rendering a CORE tool is as legitimate as a plugin tool: keyed by
// name, source-agnostic. Takes precedence over a display-kind renderer. A
// renderer for a tool whose plugin is disabled (or for a name nothing
// registers) simply never matches -- the fallback is not an error.
class ToolResultRenderer {
public:
    virtual ~ToolResultRenderer() = default;
    virtual std::string_view tool_name() const = 0;
    virtual ftxui::Element Render(const ToolResultView& result, const Theme& theme,
                                  const RowRenderContext& ctx) const = 0;
};

// --- Display-kind renderers (keyed by display payload kind) -----------------

// Draws one KIND of tool-result display payload (docs/EXTENSIBILITY.md,
// "Tool-result display payloads"). The tool decides WHAT its result is; this
// decides how that kind looks, for every tool that emits it -- keyed by kind,
// not tool name, because aliases rename tools and kinds are shared. Throw on a
// payload you cannot draw: the host contains it and falls back to the text row.
class DisplayRenderer {
public:
    virtual ~DisplayRenderer() = default;
    virtual std::string_view kind() const = 0;

    // This kind's built-in collapse default, when it has an opinion: false =
    // starts expanded (a diff, an ask's recorded answers), true = starts
    // collapsed. nullopt defers to core's tool-row default. Only the
    // least-specific tier -- ui.json and per-row toggles still win (ChatView
    // owns that resolution). Keyed by kind, so an aliased tool keeps it.
    virtual std::optional<bool> collapsed_by_default() const { return std::nullopt; }
    virtual ftxui::Element Render(const ToolResultView& result, const Theme& theme,
                                  const RowRenderContext& ctx) const = 0;
};

// --- Message-kind presentation (keyed by ChatRowKind) -----------------------

// Renderers for one kind of chat row; core supplies the defaults the
// message-kind table specifies. The DATA PLANE (event demux, virtualization,
// streaming buffer) stays core regardless of who renders a row. A plugin
// claiming a kind replaces the core default for that kind only; a second
// plugin claiming the same kind is a fatal collision (a new name resolution kind
// is overkill for an enum key -- the registry tracks (kind, plugin) pairs and
// the loader surfaces duplicates through the same fatal path).
class MessageRenderer {
public:
    virtual ~MessageRenderer() = default;
    virtual ftxui::Element Render(const ChatRow& row, const Theme& theme,
                                  const RowRenderContext& ctx) const = 0;

    // The row rendered as one or more CHUNK Elements, oldest (top) first.
    // ChatView turns every chunk into its own scroll ANCHOR, so a message
    // taller than the viewport can be scrolled through gradually instead of
    // snapping whole-message per wheel tick (the "mouse scrolls too fast"
    // complaint). The default keeps the pre-chunking contract exactly: one
    // chunk = Render(); a renderer opts in by overriding. Chunks render
    // stacked and adjacent, so splitting is safe anywhere the visual result
    // equals the unsplit render of the same content -- split at paragraph /
    // blank-line boundaries, never mid-wrap or mid-glyph.
    virtual ftxui::Elements RenderBlocks(const ChatRow& row, const Theme& theme,
                                         const RowRenderContext& ctx) const {
        return {Render(row, theme, ctx)};
    }
};

// The focused tab as the status bar sees it, computed by the host once per
// frame and handed to every segment. All empty/zero when no tab is focused
// (`focused` false); a tab without a session yet has an empty session_id.
struct TabStatus {
    bool focused = false;
    std::string session_id;
    std::string display_name;   // "" until the session is named
    bool read_only = false;     // a finished subagent's transcript tab
    std::int64_t tokens_used = 0;      // context occupancy: input + output + cache
    bool tokens_estimated = false;     // tokens_used is an estimate, not a backend count
    std::int64_t context_window = 0;   // 0 = unknown
    // Effective values (what the session runs with) and desired ones (the
    // user's last choice, staged until the server confirms it; "" = none).
    std::string model, desired_model;
    std::string agent_preset, desired_agent_preset;
    std::string permission_mode, desired_permission_mode;
    bool paused = false;
    // "idle", "thinking", "working", "waiting", "error" or "done".
    std::string state;
    std::string running_tool;          // the first tool running, while "working"
    bool cancel_requested = false;     // interrupted, the turn not over yet
    bool live_view_truncated = false;  // older live rows of this turn were dropped
};

// Where a segment sits: left of the bar's split or right of it, ascending
// by order within a side (ties by id). Orders are plugins' own judgment
// calls, spaced (10, 20, ...) so another plugin can land between.
struct StatusPlacement {
    enum class Side { kLeft, kRight };
    Side side = Side::kLeft;
    int order = 0;
};

// One segment's value for this frame: its text ("" = renders nothing) and
// the theme role that tints it ("" = the bar's own color).
struct StatusValue {
    std::string text;
    std::string role;
};

// --- Status-bar segments (keyed by segment id) ------------------------------

// One provider per segment id. A segment places itself (placement(): a side
// of the bar and an order within it) and the host assembles the bar from
// what is registered, so enabling a plugin is enough to show its segment;
// ui.json's `status_segments` hides or moves any of them by id, and
// `minimal status-bar` prints the result.
//
// Render() is called on the render thread every frame (up to ~20 Hz while
// anything changes): it must be cheap and must never block -- a segment that
// needs live data (a plugin RPC, say) fetches it on its OWN background thread
// and has Render() read a cached answer, never call out synchronously from
// here. Frames are event-driven: when that thread's data changes, it asks for
// one through the context's HostUi (RequestRedraw). Color comes from theme
// roles, never hardcoded; the v1 contract is text (the core segments are
// strings too). The focused tab's state (TabStatus) travels with every call,
// the render-time input rule -- a segment that cares about THE focused tab
// (a per-session token/cost readout, say) reads it there.
class StatusSegment {
public:
    virtual ~StatusSegment() = default;
    virtual std::string_view id() const = 0;
    // Runs every frame, on the UI thread: cheap, never blocking.
    virtual StatusValue Render(const TabStatus& focused) const = 0;
    // Where it sits when ui.json does not say otherwise; nullopt: shown only
    // where ui.json's status_segments places it. Separate from priority():
    // where a segment sits is not what gives way first.
    virtual std::optional<StatusPlacement> placement() const { return std::nullopt; }
    // Which segment gives way first when the bar overflows: lowest first.
    virtual int priority() const { return kPluginStatusSegmentPriority; }
    // A value wider than this many columns is truncated -- and, short of
    // room, truncated further before anything else gives way. 0: never
    // truncated; it is dropped whole by priority instead.
    virtual std::size_t max_columns() const { return 0; }
};

// --- Syntax grammars (additive, keyed by language) --------------------------

// One language grammar: the vendored set registers as the defaults, and a
// plugin may add a language (a grammar ships its own TSLanguage* -- the
// vendored symbols are not exported). Two plugins claiming the same language
// is a fatal collision (a name resolution kind). The rule type is the
// highlighter's own `client::SyntaxRule`; no translation between ABI and core.
class SyntaxGrammar {
public:
    virtual ~SyntaxGrammar() = default;
    // Canonical language key ("python"), matched against a fence label
    // before any language's Aliases().
    virtual std::string_view Language() const = 0;
    // The tree-sitter parse function itself (tree_sitter_python and friends) --
    // the raw symbol, not a called result, so the highlighter's parser setup
    // treats registered and vendored grammars identically.
    virtual minimal::client::LanguageFnPtr LanguageFn() const = 0;
    virtual std::span<const SyntaxRule> Rules() const = 0;
    // Positions that name a function (a call target, a declaration's name),
    // which the node-type table alone cannot tell from other identifiers.
    virtual std::span<const FunctionFieldRule> FunctionFields() const { return {}; }
    // Other fence labels that mean this language ("py" for "python"). A
    // language's own name always wins over another's alias.
    virtual std::span<const std::string_view> Aliases() const { return {}; }
};

// --- Markdown engine (EXCLUSIVE) --------------------------------------------

// The exclusive replacement slot for the whole markdown pipeline. The contract
// carries the streaming semantics explicitly: complete-text render AND the
// streaming lifecycle, whose debounce-and-finalize property ("partial input
// cannot soundly parse"; Finalize() always reparses authoritatively) is the
// IMPLEMENTER'S obligation, not the host's. Second registration warns and is
// ignored; with none registered the core md4c adapter stands.
class MarkdownEngine {
public:
    virtual ~MarkdownEngine() = default;

    // A persisted (complete) block.
    virtual ftxui::Element Render(std::string_view markdown_text, const Theme& theme) const = 0;

    // The in-flight block. The implementer owns debounce and finalize: Render
    // may reparse at the implementer's cadence, Finalize() must produce the
    // authoritative render of everything appended so far.
    class Stream {
    public:
        virtual ~Stream() = default;
        virtual void AppendDelta(std::string_view text) = 0;
        virtual void Finalize() = 0;
        virtual ftxui::Element Render(const Theme& theme) const = 0;
    };
    virtual std::unique_ptr<Stream> MakeStream() const = 0;
};

}  // namespace minimal::client::plugins
