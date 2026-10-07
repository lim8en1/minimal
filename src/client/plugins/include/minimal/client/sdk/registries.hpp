#pragma once

// The client slot registries (docs/EXTENSIBILITY.md 4.2) -- the TUI's pluggable
// presentation surface, mirroring the server's registry pattern:
//
//   - ADDITIVE registries (views, tool-result renderers, message renderers,
//     status segments, syntax grammars) take many plugins; a duplicate key is
//     recorded through the shared name resolution and is FATAL at startup, naming
//     both claimants.
//
//   - EXCLUSIVE registries (chat surface, markdown engine) take one; a second
//     registration warns and is ignored -- the MentionResolver/AuthProvider
//     pattern. The core default (registered only when no plugin claimed the
//     slot) is what "exclusive" protects.
//
//   - Every registry carries the server's attribution window trio
//     (SetAttribution / ClearAttribution / attributions) so a collision can
//     name both culprits and a PluginRecord can say what one plugin registered.
//
// CONTRACT: registration-time-only, now ENFORCED -- every registry is sealed
// once loading finishes (TuiEnvironment::SealRegistries), and a later
// Register warns and is ignored instead of silently bypassing the collision
// rule, the attribution windows, and (for exclusive slots) the core default.
// Still single-threaded: nothing concurrent ever registers.
//
// Render-time inputs travel with the call -- including `const Theme&`. Plugins
// load BEFORE the theme does (theme `sections` need to know which plugins are
// loaded), so nothing renderable can capture a theme at registration.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/subscribe_outcome.hpp"
#include "minimal/plugins/name_table.hpp"
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include <tree_sitter/api.h>

#include "minimal/client/sdk/chat_model.hpp"
#include "minimal/client/sdk/command_catalog_entry.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/sdk/highlighter.hpp"
#include "minimal/client/sdk/widget_types.hpp"
#include "minimal/v1/session_service.pb.h"
#include "minimal/client/policy.hpp"

namespace minimal::client::plugins {

// The highlighter's rule type IS the ABI type -- no translation between a
// registered grammar and core.
using SyntaxRule = minimal::client::SyntaxRule;
using LanguageFnPtr = minimal::client::LanguageFnPtr;

// --- Attention (docs/EXTENSIBILITY.md 4.2, "Attention sink") ----------------

// What a plugin may raise or clear. Core event derivation (the Waiting '!') is
// the default source; a plugin raise WINS until cleared, and the glyph is the
// same '!': the only color-independent signal, whatever raised it.
//
// Implemented by the host over a shared attention state block; handed to
// plugins in the registration context.
// The host UI seam. The frame repaints only when the app's own state moved
// (the pump is event-driven), so a plugin whose data changed on its OWN thread
// -- a feed landing new totals, a picker's list arriving -- showed nothing
// until some unrelated event happened to redraw. RequestRedraw asks for the
// next frame. Thread-safe, never blocks, cheap (a counter bump); calling it
// with nothing new is harmless.
// One key binding as the TUI has it: the action or view it triggers, the
// key, and -- core's vocabulary -- what it does and where it belongs.
struct KeyBindingInfo {
    std::string id;           // "chat.scroll_up", or a plugin view's id
    std::string key;          // "ctrl+alt+k"
    std::string description;  // "scroll the chat up"
    std::string group;        // "chat", ..., "plugins"
    int order = 0;            // within the group, as core lists it
};

// One tool call a tab has seen, as the tools panel shows it. Running ones
// and the recent finished ones; the persisted transcript is the record.
struct ToolActivity {
    std::string name;                     // "tool" when the server sent none
    std::string summary;                  // one line: the tool's own, or its input's
    bool running = false;
    bool failed = false;                  // finished with an error
    std::int64_t started_at_unix_ms = -1; // -1 = unknown
    std::int64_t duration_ms = -1;        // -1 while running (or unknown)
    // Its live output so far: lossy (head and tail of a capped buffer),
    // shared and immutable. Null when there is none.
    std::shared_ptr<const std::string> output;
    std::string tool_use_id;
    // The model's call arguments, shared and immutable; never null.
    std::shared_ptr<const nlohmann::json> input = std::make_shared<const nlohmann::json>();
};

// One tab, for views that show every tab (the tools panel).
struct TabActivity {
    std::string tab_id;
    std::string session_id;     // "" while the tab has no session
    std::string display_name;
    // "idle", "thinking", "working", "waiting", "error" or "done".
    std::string state;
    bool attention = false;     // waiting on the user, or raised by a plugin
    bool focused = false;
    bool subagent = false;
    bool view_only = false;     // a transcript tab: nothing to send on
    std::int64_t root_ordinal = 0;   // which root session's tree, in open order
    std::uint32_t spawn_seq = 0;     // within it: 0 the root, then children
    std::vector<ToolActivity> tools;
};

// Every open tab, in tab order. `version` moves only when something here
// changed: a view keyed on it rebuilds on change, not on every frame.
struct TabActivitySnapshot {
    std::uint64_t version = 0;
    std::vector<TabActivity> tabs;
};

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

class HostUi {
public:
    virtual ~HostUi() = default;
    virtual void RequestRedraw() = 0;
    // A frame no later than `delay` from now -- for state that changes with
    // time and no event (a debounced reparse coming due). The earliest of
    // several requests wins. Any thread. (The default draws one now.)
    virtual void RequestRedrawAfter(std::chrono::milliseconds delay) {
        (void)delay;
        RequestRedraw();
    }
    // View control, by registered view id. Callable from any thread: each is
    // queued and applied on the UI thread before the next frame (in call
    // order). An id no view answers to is a no-op.
    // Shows a panel (and focuses it) or opens a modal (replacing any other
    // plugin modal) -- the route a plugin's own /-command takes to its UI.
    virtual void ShowView(const std::string& view_id) = 0;
    // Hides a panel, or closes a modal if it is the one showing.
    virtual void CloseView(const std::string& view_id) = 0;
    // Panels only: show (and focus) or hide.
    virtual void SetPanelVisible(const std::string& view_id, bool visible) = 0;
    // Panels only, and only a visible one: it receives key events next.
    virtual void FocusPanel(const std::string& view_id) = 0;

    // --- Reads: what the TUI knows. UI thread only (render, an event, a
    // view's OnShow), where the host's own state is consistent.

    // Every key binding in force -- core's actions and the plugin views' --
    // with what core says each does: its own vocabulary, the plugin view's
    // title for a view.
    virtual std::vector<KeyBindingInfo> KeyBindings() const = 0;
    // The /-commands the focused tab offers: the server's (for the tab's
    // project and session), the client's and the plugins', filtered as the
    // prompt's autocomplete filters them -- without a session only what
    // runs without one, on a subagent only what is read-only there.
    virtual std::vector<CommandCatalogEntry> CommandCatalog() const = 0;
    // The themes there are to switch to (the themes dir's files, by name).
    virtual std::vector<std::string> ThemeNames() const = 0;
    // This plugin's own persisted state: what SaveState last stored, across
    // restarts (null when nothing was). A small JSON document per plugin in
    // the data dir; for preferences a view flips at run time. UI thread.
    // (The defaults: nothing kept.)
    virtual nlohmann::json LoadState() const { return nullptr; }
    virtual void SaveState(const nlohmann::json& state) { (void)state; }
    // Every tab and its tool calls. Never null. (The default: no tabs.)
    virtual std::shared_ptr<const TabActivitySnapshot> Activity() const {
        static const auto kNone = std::make_shared<const TabActivitySnapshot>();
        return kNone;
    }
};

// --- Host actions -------------------------------------------------------------

// What a plugin asks the TUI to do, instead of touching its state: verbs on
// the host's own model. Callable from any thread; each is queued and applied
// on the UI thread before the next frame, in call order. A verb whose target
// is gone by then (no tab) does nothing.
class HostActions {
public:
    virtual ~HostActions() = default;
    // Replaces the focused tab's prompt with `text`, the caret at its end
    // (the commands overlay's pick: "/name ", ready for arguments).
    virtual void InsertPrompt(const std::string& text) = 0;
    // Switches the UI theme to `name` (themes/<name>.json) and remembers it
    // for the next start; a theme that fails to load changes nothing and
    // says why in the focused tab's notice.
    virtual void SetTheme(const std::string& name) = 0;

    // The tab's desired agent preset / model -- what its next message runs
    // with (a sessionless tab's session is created on it; a sessioned one
    // converges at the next turn), with the tab notice saying so. Lands on
    // the focused tab, or a fresh one on the empty app. Each also continues
    // a first message that was waiting on the choice ("agents" / "models"
    // views opened for it).
    virtual void ChooseAgent(const std::string& name) = 0;
    virtual void ChooseModel(const std::string& ref) = 0;
    // Back to the default model; `default_ref` is the server's name for it
    // (ListModelsResponse::default_model), shown until the session confirms.
    virtual void ChooseDefaultModel(const std::string& default_ref) = 0;
    // The focused session's subagent model: a `<provider>/<model>` ref, or
    // "inherit" (children run the parent's model).
    virtual void SetSubagentModel(const std::string& ref) = 0;

    // A fresh tab without a session (its first message creates one), focused.
    virtual void NewTab() = 0;
    // Opens a stored session in a tab and focuses it: attached when it is
    // live (`is_live`, as the session list said), resumed from its
    // transcript otherwise. A session already open in a tab just focuses it.
    virtual void OpenSession(const std::string& session_id, bool is_live) = 0;
    // A subagent of `parent_session_id`: an open tab is focused, a live one
    // attached into its parent's tab group, an ended one opened read-only.
    virtual void OpenSubagent(const std::string& session_id, const std::string& parent_session_id,
                              bool is_live) = 0;
};

class AttentionSink {
public:
    virtual ~AttentionSink() = default;
    virtual void RaiseTab(const std::string& session_id) = 0;
    virtual void ClearTab(const std::string& session_id) = 0;
};

// The host-side state behind AttentionSink, shared by the sink and AppState.
// Created before plugin load; AppState reads it per frame. Thread-safe: Raise
// arrives from plugin code (often a plugin's data thread), reads happen on the
// render thread.
class AttentionState {
public:
    void RaiseTab(const std::string& session_id);
    void ClearTab(const std::string& session_id);

    // A plugin raise wins until cleared; core derivation ORs in underneath.
    bool TabRaised(const std::string& session_id) const;
    std::vector<std::string> RaisedTabs() const;

private:
    mutable std::mutex mtx_;
    std::map<std::string, bool> tab_;
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

// --- Prompt renderers (keyed by plugin prompt kind) -------------------------

// What a prompt renderer answers with -- the PluginPromptResponse minus the
// request id, which the host fills in. `answer` is sent only with ANSWERED.
struct PromptReply {
    v1::PromptOutcome outcome = v1::PROMPT_OUTCOME_DISMISSED;
    nlohmann::json answer = nullptr;
};

// Everything a prompt renderer builds its modal FROM. Valid only for the
// Make() call: a component copies whatever it keeps.
struct PromptView {
    std::string_view kind;
    // The server plugin's request, parsed from PluginPromptRequest.payload_json.
    const nlohmann::json& payload;
    // Who is asking, for the modal's wording: the tab's agent label ("" for
    // the main agent, e.g. "review" on a subagent's tab).
    std::string_view asker;
};

// Draws one KIND of prompt as a modal: a plugin kind (docs/EXTENSIBILITY.md,
// "Plugin prompts" -- the client half of a server plugin that calls
// ToolContext::prompt_user), or one of core's own two, "permission" and
// "confirmation" (§4.2: their payloads and answers, both kinds required at
// startup). The host shows it in its modal layer, rebuilds it per request id,
// and sends whatever `on_reply` receives. A kind with no renderer, or a Make()
// that throws, is answered at once -- UNAVAILABLE for a plugin kind, deny /
// "do not proceed" for core's -- never left hanging.
class PromptRenderer {
public:
    virtual ~PromptRenderer() = default;
    virtual std::string_view kind() const = 0;
    // `theme` outlives the component. Call `on_reply` once; the host ignores
    // any later call for the same request (the slot is gone by then).
    virtual ftxui::Component Make(const PromptView& prompt, const Theme& theme,
                                  std::function<void(PromptReply)> on_reply) const = 0;
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

// --- Status-bar segments (keyed by segment id) ------------------------------

// One provider per segment id; the format string (ui.json `status_format`)
// orders and selects by id, so a plugin adds content while the user keeps
// layout control. An unknown id renders nothing with a startup warning --
// the keybindings.json rule for names unknown at compile time.
//
// Render() is called on the render thread every frame (up to ~20 Hz while
// anything changes): it must be cheap and must never block -- a segment that
// needs live data (a plugin RPC, say) fetches it on its OWN background thread
// and has Render() read a cached answer, never call out synchronously from
// here. Frames are event-driven: when that thread's data changes, it asks for
// one through the context's HostUi (RequestRedraw). Color comes from theme
// roles, never hardcoded; the v1 contract is text (the core segments are
// strings too). `focused_session_id` is the render-time input every other
// registry already takes (see the file banner) -- a segment that cares about
// THE focused tab (a per-session token/cost readout, say) has no other way to
// learn which one that is; empty when no tab is focused.
// The focused tab as the status bar sees it, computed by the host once per
// frame and handed to every segment. All empty/zero when no tab is focused
// (`focused` false); a tab without a session yet has an empty session_id.
struct TabStatus {
    bool focused = false;
    std::string session_id;
    std::string display_name;   // "" until the session is named
    bool read_only = false;     // a finished subagent's transcript tab
    std::int64_t tokens_used = 0;      // context occupancy: input + output + cache
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

// One segment's value for this frame: its text ("" = renders nothing) and
// the theme role that tints it ("" = the bar's own color).
struct StatusValue {
    std::string text;
    std::string role;
};

class StatusSegment {
public:
    virtual ~StatusSegment() = default;
    virtual std::string_view id() const = 0;
    // Runs every frame, on the UI thread: cheap, never blocking.
    virtual StatusValue Render(const TabStatus& focused) const = 0;
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

// --- Widget kit (EXCLUSIVE) ------------------------------------------------

// The standard widgets views are built from: the filterable menu every
// picker is, the schema form, the modal chrome. A view asks the kit
// (ClientPluginRegistrationContext::widgets) rather than building its own,
// so replacing the kit -- disable the default, register another -- restyles
// every view built on it at once. The contracts (rows, specs, callbacks,
// state) are view/widget_types.hpp; a kit owns only how they look and
// behave. Core registers its own default into an unclaimed slot.
//
// The theme comes with every call: plugins load before the theme does.
class WidgetKit {
public:
    virtual ~WidgetKit() = default;

    // A type-to-filter list in a modal window. `build_rows` runs on every
    // keystroke and every rebuild: cheap and side-effect free.
    virtual ftxui::Component FilterableMenuModal(
        std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
        const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks,
        const Theme& theme) const = 0;

    // A form over a config schema (SchemaFormState::describe). `source` is
    // read on every render and event: cheap. The component implements
    // EscapeAware -- its inline editor wants Escape while it is open.
    virtual ftxui::Component SchemaForm(std::function<SchemaFormState()> source,
                                        SchemaFormCallbacks callbacks,
                                        const Theme& theme) const = 0;

    // A titled modal window: `rows` scroll within the screen's height,
    // `pinned` stays visible below them (details, the last outcome), and
    // `footer` -- the key hints -- is pinned last. Empty parts are omitted.
    // `accent`: whether the user is being asked something (ModalAccent).
    virtual ftxui::Element ModalFrame(const Theme& theme, const std::string& title,
                                      ftxui::Elements rows, ftxui::Elements pinned,
                                      const std::string& footer, ModalAccent accent) const = 0;

    // A divider between sections of a modal's body.
    virtual ftxui::Element ModalRule(const Theme& theme) const = 0;

    // A model chooser that fetches the list itself through `spec.fetch`,
    // on the caller's worker (`spec.post`); ctrl+r refreshes discovery.
    virtual ftxui::Component ModelSelector(ModelSelectorSpec spec,
                                           ModelSelectorCallbacks callbacks,
                                           const Theme& theme) const = 0;
};

// --- Prompt editor (EXCLUSIVE, REQUIRED) -----------------------------------

// The submitted-line timeline, app-scoped (every tab's editor records into
// and recalls from the same one) and persisted by core. Entries are numbered
// by a sequence that never repeats: entry i is FirstSeq() + i, and the
// oldest entries fall off the front, so an editor's browse position is a
// sequence number -- one below FirstSeq() has been trimmed away.
class PromptTimeline {
public:
    virtual ~PromptTimeline() = default;
    virtual const std::vector<std::string>& Entries() const = 0;
    virtual std::uint64_t FirstSeq() const = 0;
    // A submitted line. Core decides what of it is kept (a command whose
    // arguments may carry a credential is kept as its bare name) and skips a
    // repeat of the newest entry.
    virtual void Record(const std::string& line) = 0;
};

// What core gives the prompt editor. Every call on the UI thread; every
// call acts on the FOCUSED tab -- the editor shown is always that tab's.
class PromptEditorHost {
public:
    virtual ~PromptEditorHost() = default;
    // The two submit paths: a turn, or a /-command line (the leading '/'
    // included). Which one a line is, is the editor's call -- SubmitLine
    // below is the usual rule.
    virtual void SubmitChat(const std::string& text) = 0;
    virtual void SubmitCommand(const std::string& line) = 0;
    // The /-commands usable on the focused tab now (core has applied the
    // session and subagent gates), or null before the catalog is known. The
    // pointer changes exactly when the list does: cheap to poll per frame.
    virtual std::shared_ptr<const std::vector<CommandCatalogEntry>> Commands() = 0;
    // @-mention completion: the paths matching `prefix`. nullopt = still
    // being fetched -- ask again on a later frame; a value (possibly empty)
    // is the answer, handed out once.
    virtual std::optional<std::vector<std::string>> Mentions(const std::string& prefix) = 0;
    virtual PromptTimeline& History() = 0;
    // Whether `event` is bound to `action` -- one of the editor's own
    // (PromptEditorFactory::Actions), as keybindings.json resolved it.
    virtual bool Matches(std::string_view action, const ftxui::Event& event) const = 0;
    // Asks for a frame (any thread): a timer the editor runs from render
    // needs frames to keep coming.
    virtual void RequestRedraw() = 0;
};

// The usual routing: a line starting with '/' is a command.
inline void SubmitLine(PromptEditorHost& host, const std::string& line) {
    if (!line.empty() && line.front() == '/') {
        host.SubmitCommand(line);
    } else {
        host.SubmitChat(line);
    }
}

// One tab's editor. Core keeps one per tab, made on the tab's first frame
// and dropped with the tab.
class PromptEditor {
public:
    virtual ~PromptEditor() = default;
    // Built once; the host swaps the focused tab's in.
    virtual ftxui::Component component() = 0;
    virtual std::string Text() const = 0;
    // Replaces the text, caret at the end (a help entry inserted, a message
    // that failed to send handed back).
    virtual void SetText(const std::string& text) = 0;
    // Asked BEFORE core's global key bindings: true keeps `event` from any
    // binding, so it reaches the editor (an open suggestion list's keys, a
    // pasted byte that looks like a bound key, a modal editor's Escape).
    virtual bool Claims(const ftxui::Event& event) const = 0;
    // Any thread: the editor is waiting on time to pass (a debounce) and
    // needs frames meanwhile.
    virtual bool WantsFrames() const = 0;
    // A modal took the keyboard: drop state a key sequence left half-done (a
    // paste whose end marker the modal will swallow).
    virtual void OnFocusLost() {}
};

// A key action a slot implementation declares (the prompt editor's, the chat
// surface's): registered into the keymap at startup -- `default_key` only
// where keybindings.json binds nothing -- and listed in the keymap help with
// `description`. Ids are prefixed by convention ("prompt.", "chat.").
struct DeclaredAction {
    std::string id;
    std::string default_key;
    std::string description;
};

class PromptEditorFactory {
public:
    virtual ~PromptEditorFactory() = default;
    virtual std::vector<DeclaredAction> Actions() const = 0;
    // `host` and `theme` outlive the editor.
    virtual std::unique_ptr<PromptEditor> Make(PromptEditorHost& host,
                                               const Theme& theme) const = 0;
};

// The chat surface (EXCLUSIVE, REQUIRED): defined after the renderer
// registries its host hands out (see below).
class ChatSurface;


// --- Views (additive: panel or modal) ---------------------------------------

enum class ViewPlacement { Panel, Modal };

struct ViewSpec {
    // The canonical id: what the plugin's own code addresses (HostUi view
    // requests). Users address the SURFACE name -- this id, or the
    // client-plugins.json alias that renamed it (keybindings.json, view hints).
    std::string id;
    std::string title;
    ViewPlacement placement = ViewPlacement::Panel;
    // Registered as the DEFAULT binding only if the user's keybindings.json
    // does not already name this id (Keybindings::EnsureDefault).
    std::string default_keybinding = {};
};

// A self-contained view. Panels render in the panel region; modals render in
// the modal layer -- BELOW the quit confirmation and the permission modal,
// always: a plugin modal cannot preempt or mask the security UI (the invariant
// is structural in the host's modal nesting, and tested).
class View {
public:
    virtual ~View() = default;
    virtual const ViewSpec& spec() const = 0;
    virtual ftxui::Component component() = 0;

    // Called once after the theme is loaded (views register before it), the
    // same push ChatSurface gets. Without it the interface's own "colors come
    // from theme roles" contract (see plugin_abi.h's View docs) is
    // unimplementable: a view has no other route to the resolved theme.
    // Default no-op so views that hardcode styles keep compiling. A throw
    // here disables nothing yet (the host catches); render-time calls are
    // contained separately.
    virtual void OnTheme(const Theme& theme) { (void)theme; }

    // Called on the UI thread each time the host makes the view visible (a
    // command's view_hint, or its keybinding toggling it on) -- the moment a
    // view over live server state (the mcp picker) starts a fresh fetch. Must
    // not block: kick background work and return. Default no-op. A throw is
    // contained by the host (logged; the view still opens). Const, like the
    // rest of the dispatch surface (PluginViewEntry holds a const View*): a
    // view keeps whatever it kicks behind its own pointer members.
    virtual void OnShow() const {}
};

// A registered view's runtime dispatch entry: the view's own spec, its
// (already-constructed) component, and whether it is currently visible
// (panels toggle; modals stage into the host's modal layer on open). `view`
// is a non-owning pointer into a ViewRegistry's heap-owned Views -- safe
// regardless of the registry's own address, since a registry only ever
// relocates its OWN vector of unique_ptrs, never the pointees. Lives here
// (not in the client's bootstrap/dispatch code) so both the startup sequence
// that builds the list and the global-keybinding dispatch that reads it can
// share one definition without one including the other.
struct PluginViewEntry {
    const View* view;
    ftxui::Component component;
    bool visible = false;
    // The panel-focus rule (keybinding_dispatch.cpp): at most ONE visible
    // panel is focused; it alone receives key events, and the frame renders
    // its chrome (border + title). Opening a panel takes focus; closing the
    // focused one moves focus to the first remaining visible panel. Modals
    // never use it (they stage into the modal layer whole).
    bool focused = false;
    // What keybindings and view hints address: the client-plugins.json alias
    // that renamed the view, else spec().id.
    std::string surface_id;
    // The registering plugin ("" for core): with spec().id, what that plugin's
    // own view requests address (ViewRequest::plugin).
    std::string plugin;
};

// --- Start views (additive: the initial view on start) ----------------------

// The initial view rendered while there is no conversation content yet: the
// app-level empty state (no tab open) and a freshly opened session before its
// first message arrives. Additive -- every registered start view renders,
// stacked in registration order; a duplicate id is fatal through the shared
// name resolution like every other view id.
//
// Element, not Component: a start view is non-interactive decoration, which
// keeps the 20 Hz render contract trivially cheap. The per-render session id
// is what lets a plugin vary its art per session (a generator keyed by
// session_id -- regenerating inside Render() with no key would re-randomize
// every frame at 20 Hz).
class StartView {
public:
    virtual ~StartView() = default;
    virtual const ViewSpec& spec() const = 0;
    // `session_id` empty = the app-level empty state (no tab open). Colors
    // come from theme roles, never hardcoded (the same contract as every
    // other render seam); missing roles fall back to Color::Default.
    virtual ftxui::Element Render(const std::string& session_id, const Theme& theme) const = 0;
};

// --- The sanctioned data path ------------------------------------------------

// Implemented by the host over SessionClient. Declared here so the ABI names
// the capability, not the transport. A view or chat surface NEVER opens its
// own connection; the host owns the channel.
class ClientApi {
public:
    virtual ~ClientApi() = default;

    struct MessagePage {
        std::vector<v1::StoredMessage> messages;  // descending by seq
        bool has_more = false;
    };

    // Keyset page over the session transcript -- the same call, cursor
    // semantics and bound core's scrollback uses (nullopt cursor = newest
    // page). Bounded like core's own refresh (2 s deadline). nullopt return:
    // the call failed or timed out -- a transient condition, not an error to
    // surface.
    virtual std::optional<MessagePage> GetMessages(const std::string& session_id,
                                                   std::optional<std::int64_t> before_seq,
                                                   int limit) = 0;

    // The generic plugin channel (docs/EXTENSIBILITY.md 2.5, 4.3 ruling 3):
    // a plugin's OWN data path, plugin-owned JSON both ways. `plugin_name`
    // names the server-side pair; core transports and never interprets.

    // One call, one JSON answer. nullopt: refused, unknown pair, or endpoint
    // failure -- the transport-level conditions. Plugin-level errors are
    // plugin-owned JSON, not this nullopt.
    virtual std::optional<std::string> PluginCall(const std::string& plugin_name,
                                                  const std::string& method,
                                                  const std::string& json_payload) = 0;

    // The models the server can run: every configured or discovered
    // `<provider>/<model>` ref, and the default when there is one.
    // `refresh_discovery` re-fetches every discovering provider's catalog
    // first -- seconds, not milliseconds. Blocking: call it from a worker
    // (TaskWorker), never the UI thread. nullopt: the call failed, and
    // `error` (when given) says why, for the plugin to show.
    virtual std::optional<v1::ListModelsResponse> ListModels(bool refresh_discovery,
                                                             std::string* error) = 0;

    // The agent presets for this client's project, and the default when
    // there is one. Blocking, like ListModels.
    virtual std::optional<v1::ListAgentPresetsResponse> ListAgentPresets(std::string* error) = 0;

    // One page of the stored sessions -- this client's project's, or every
    // project's -- newest first, top-level only. `before_*` is the keyset
    // cursor (the last row of the previous page; unset = the first page).
    // Blocking, like ListModels.
    struct SessionQuery {
        bool include_all_dirs = false;
        int limit = 0;  // 0: the server's page size
        std::optional<std::int64_t> before_updated_at;
        std::optional<std::string> before_id;
    };
    virtual std::optional<v1::ListSessionsResponse> ListSessions(const SessionQuery& query,
                                                                 std::string* error) = 0;
    // Every session `parent_session_id` spawned, live or ended, across all
    // pages. Blocking.
    virtual std::optional<std::vector<v1::SessionSummary>> ListSubagents(
        const std::string& parent_session_id, std::string* error) = 0;
    // Deletes a stored session (its transcript and its subagents'),
    // permanently. Blocking. False, with `error`, when the server refused --
    // a session that is open somewhere stays.
    virtual bool DeleteSession(const std::string& session_id, std::string* error) = 0;

    // Streams the subscription to `on_payload` on the CALLING thread (blocking
    // until the server closes the stream or the callback returns false).
    // Tri-state outcome: kCancelled = the subscriber stopped; kEnded = the
    // server closed a live subscription; kError = refused or transport death
    // (the host logs the status).
    virtual SubscribeOutcome PluginSubscribe(
        const std::string& plugin_name, const std::string& method,
        const std::string& json_payload,
        const std::function<bool(const std::string&)>& on_payload) = 0;
};

// --- Focus context (which tab/session the user is looking at) ---------------

// Status segments receive their render-time session id as a Render argument;
// COMPONENTS (panel views, chat surfaces) are self-contained and have no
// other route to it. This is that route -- the focused/summary split a
// session-aware panel needs, and the display-name lookup the .rpc channel
// cannot answer (names are client-side tab state, not server data).
//
// Null in the registration context = the host does not offer the seam (a
// test exercising other registries, or a host built before the seam): a
// plugin treats that as "no focus knowledge" -- degraded content, not an
// error -- the same rule `attention` and `client_api` follow.
//
// THREADING: callable from any thread (a plugin's background poller reads
// focus between fetches and refetches when it changed). Cheap and
// non-blocking: the host answers from in-memory state, never over the wire.
struct TabInfo {
    std::string tab_id;        // the UI's tab handle (AppState's tab id)
    std::string session_id;    // "" while the tab has no session
    std::string display_name;  // the tab bar's name for it
};

class FocusProvider {
public:
    virtual ~FocusProvider() = default;

    // The focused tab's session id: "" when no tab is focused or the focused
    // tab has no session yet.
    virtual std::string FocusedSessionId() const = 0;

    // Every open tab, focused first, in the host's own render order;
    // sessionless tabs included (their session_id is "").
    virtual std::vector<TabInfo> Tabs() const = 0;
};

// --- Observers (typed host events) -------------------------------------------

// A tab's session metadata as merged so far (a partial re-emit never blanks a
// field): what OnSessionMetadata reports when any of it changes.
struct SessionMetadata {
    std::string name;
    std::string model;
    std::string agent_preset;
    std::string permission_mode;
    bool operator==(const SessionMetadata&) const = default;
};

// What happened in the host, pushed instead of polled: a plugin that tracked
// focus by scanning Tabs() on a heartbeat (or by noticing which session a
// status segment was asked to render) reacts to the event instead.
//
// THREADING: every call arrives on the UI thread, between frames, in the
// order the host observed the changes -- never concurrently with another
// observer call. Keep them cheap (they delay the next frame); hand anything
// slow to the plugin's own thread. Events are derived from the host's state
// between frames, so two changes inside one frame arrive together, still in
// order. A throw is contained and logged; an observer that keeps throwing is
// disabled. Every method has a no-op default -- override what you need.
class Observer {
public:
    virtual ~Observer() = default;

    // A tab appeared (its session_id may still be "": a tab gets its session
    // on its first message -- OnFocusChanged / OnSessionMetadata report it).
    virtual void OnTabOpened(const TabInfo& tab) { (void)tab; }
    // A tab closed; `tab` as last seen (its session, if it had one).
    virtual void OnTabClosed(const TabInfo& tab) { (void)tab; }
    // The focused tab changed, or the focused tab's session did. All fields
    // "" when no tab is focused.
    virtual void OnFocusChanged(const TabInfo& focused) { (void)focused; }
    // A turn began on `tab`'s session (a submit, or a server-started turn).
    virtual void OnTurnStarted(const TabInfo& tab) { (void)tab; }
    // That turn ended: `completed` false when it died (an error, the session
    // ending) rather than completing (any stop reason, cancellation included).
    virtual void OnTurnCompleted(const TabInfo& tab, bool completed) {
        (void)tab;
        (void)completed;
    }
    // `tab`'s session metadata changed (also once when first known).
    virtual void OnSessionMetadata(const TabInfo& tab, const SessionMetadata& metadata) {
        (void)tab;
        (void)metadata;
    }
};

// A client-side /-command a plugin contributes: it runs in the TUI and never
// reaches the server (compare a server plugin's command, which runs on the
// session worker). Listed in /-autocomplete and the /help overlay beside the
// server's commands; a name the client's own commands already use is ignored
// with a startup warning. Run() executes on the UI thread: cheap, never
// blocking -- hand slow work to the plugin's own thread, then RequestRedraw or
// ShowView through the context's HostUi.
class ClientCommand {
public:
    virtual ~ClientCommand() = default;
    virtual std::string_view name() const = 0;      // without the leading '/'
    virtual std::string_view summary() const = 0;   // the autocomplete line
    virtual bool requires_session() const { return false; }
    // Changes nothing a session holds (it opens a view, switches the theme):
    // offered on a subagent tab, whose command surface is read-only.
    virtual bool read_only() const { return false; }
    // False: the command only intercepts some forms of a SERVER command of
    // the same name (the bare `/model` that opens a picker, while
    // `/model <ref>` stays the server's) -- not listed on its own; the
    // catalog lists the server's entry.
    virtual bool listed() const { return true; }
    // `positional`: whitespace-split arguments; `raw`: everything after the
    // name, untouched. Returns whether it handled the line; false passes it
    // on to the server's command of that name (an intercept declining a
    // form it does not own).
    virtual bool Run(const std::vector<std::string>& positional, const std::string& raw) = 0;
};

// --- Registries -------------------------------------------------------------

// Common attribution mechanics, identical to the server's registries: the
// loader opens a window around each plugin's registration callback and
// everything arriving inside it is attributed to that plugin.
struct Attribution {
    std::string plugin_name;    // empty for what core registers itself
    std::string original_name;  // the key as registered
};

// What every registry shares: the attribution window, the record of who
// registered what, rollback, and the seal.
//
// Registration-time-only enforcement, the server's Seal ported: after the
// loader has run every plugin's register_* hook, the host seals each
// registry. A plugin that stashed its registry reference and registered
// LATER used to bypass the collision rule, the attribution windows and (for
// exclusive slots) the core default entirely, silently; a sealed Register
// warns and is ignored instead.
class RegistryBase {
public:
    virtual ~RegistryBase() = default;

    void Seal() { sealed_ = true; }
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    // Every accepted registration, in order: (plugin, key). An additive
    // registry keeps a duplicate here (the collision scan's input); an
    // exclusive slot records only its holder.
    const std::vector<Attribution>& attributions() const { return attributions_; }

    // Partial-registration rollback (ClientPluginLoader::LoadOne): strips
    // everything `plugin_name` registered here, so a plugin whose LATER hook
    // throws does not leave an earlier hook's registrations live to collide
    // with a later, healthy plugin.
    virtual void RemoveByAttribution(const std::string& plugin_name) = 0;

    // Renames entries per client-plugins.json `aliases` (keyed by stable
    // identity, "plugin.original_name"). Called by the bootstrap only for
    // NAME namespaces, after the collision check passed; a slot registry is
    // never renamed. Default: nothing to rename.
    virtual void ApplyAliases(const std::map<std::string, std::string>& aliases) { (void)aliases; }
    // Nothing registered (a required slot left empty refuses startup).
    virtual bool empty() const = 0;

protected:
    // False, with a warning, once sealed.
    bool Admit() const;

    std::string attribution_;
    std::vector<Attribution> attributions_;
    bool sealed_ = false;
};

// Many plugins, one entry per key. `KeyOf` maps an entry to its key (a view
// id, a tool name, ...). A duplicate key is NOT replaced: it is kept and
// recorded, and the startup collision scan (name resolution) turns the
// duplicate fatal, naming both claimants. Each entry also has a SURFACE name
// -- its key, or the alias ApplyAliases gave it -- and Find() looks up the
// surface.
//
// FALLBACKS (RegisterFallback): a bundled default for a key that yields to
// any claim of it -- Find() answers the claim when there is one, the fallback
// otherwise, and the two never collide. Two fallbacks for one key do (they
// are recorded under "fallback:<key>").
template <typename T, typename KeyOf>
class AdditiveRegistry : public RegistryBase {
public:
    // Takes ownership. Null, or an empty key, registers nothing.
    void Register(std::unique_ptr<T> entry) {
        if (!Admit() || entry == nullptr) return;
        std::string key(KeyOf{}(*entry));
        if (key.empty()) return;
        surfaces_.push_back(key);
        owners_.push_back(attribution_);
        attributions_.push_back({attribution_, std::move(key)});
        entries_.push_back(std::move(entry));
    }

    // A default for its key that any Register() of the same key replaces.
    void RegisterFallback(std::unique_ptr<T> entry) {
        if (!Admit() || entry == nullptr) return;
        std::string key(KeyOf{}(*entry));
        if (key.empty()) return;
        attributions_.push_back({attribution_, std::string(kFallbackPrefix) + key});
        fallback_keys_.push_back(std::move(key));
        fallback_owners_.push_back(attribution_);
        fallbacks_.push_back(std::move(entry));
    }

    // The claims, in registration order (fallbacks are not listed).
    const std::vector<std::unique_ptr<T>>& All() const { return entries_; }
    bool empty() const override { return entries_.empty() && fallbacks_.empty(); }
    // Index-aligned with All(): the name each entry answers to.
    const std::vector<std::string>& surfaces() const { return surfaces_; }
    // Index-aligned with All(): the plugin that registered each entry ("" =
    // core). Not attributions(), which interleaves the fallbacks.
    const std::vector<std::string>& owners() const { return owners_; }

    // The claim for `surface`, else its fallback, else null.
    T* Find(std::string_view surface) const {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (surfaces_[i] == surface) return entries_[i].get();
        }
        for (std::size_t i = 0; i < fallbacks_.size(); ++i) {
            if (fallback_keys_[i] == surface) return fallbacks_[i].get();
        }
        return nullptr;
    }
    // Whether a claim (not a fallback) answers `surface`.
    bool Claimed(std::string_view surface) const {
        return std::find(surfaces_.begin(), surfaces_.end(), surface) != surfaces_.end();
    }

    void ApplyAliases(const std::map<std::string, std::string>& aliases) override {
        std::size_t entry = 0;
        for (const auto& attribution : attributions_) {
            if (IsFallback(attribution)) continue;
            const auto it = aliases.find(minimal::plugins::StableIdentity(
                attribution.plugin_name, attribution.original_name));
            if (it != aliases.end()) surfaces_[entry] = it->second;
            ++entry;
        }
    }

    void RemoveByAttribution(const std::string& plugin_name) override {
        // Claims: entries_ and surfaces_ are index-aligned with the claim
        // attributions (attributions_ minus the fallback ones).
        std::size_t write = 0;
        std::size_t entry = 0;
        for (const auto& attribution : attributions_) {
            if (IsFallback(attribution)) continue;
            if (attribution.plugin_name != plugin_name) {
                if (write != entry) {
                    entries_[write] = std::move(entries_[entry]);
                    surfaces_[write] = std::move(surfaces_[entry]);
                    owners_[write] = std::move(owners_[entry]);
                }
                ++write;
            }
            ++entry;
        }
        entries_.resize(write);
        surfaces_.resize(write);
        owners_.resize(write);
        write = 0;
        for (std::size_t read = 0; read < fallbacks_.size(); ++read) {
            if (fallback_owners_[read] == plugin_name) continue;
            if (write != read) {
                fallbacks_[write] = std::move(fallbacks_[read]);
                fallback_keys_[write] = std::move(fallback_keys_[read]);
                fallback_owners_[write] = std::move(fallback_owners_[read]);
            }
            ++write;
        }
        fallbacks_.resize(write);
        fallback_keys_.resize(write);
        fallback_owners_.resize(write);
        std::erase_if(attributions_, [&](const Attribution& attribution) {
            return attribution.plugin_name == plugin_name;
        });
    }

private:
    static constexpr std::string_view kFallbackPrefix = "fallback:";
    static bool IsFallback(const Attribution& attribution) {
        return attribution.original_name.rfind(kFallbackPrefix, 0) == 0;
    }

    std::vector<std::unique_ptr<T>> entries_;
    std::vector<std::string> surfaces_;
    std::vector<std::string> owners_;
    std::vector<std::unique_ptr<T>> fallbacks_;
    std::vector<std::string> fallback_keys_;
    std::vector<std::string> fallback_owners_;
};

// Warns that `claimant` lost the exclusive slot to `holder`.
void WarnTaken(const std::string& holder, const std::string& claimant);

// One holder. Every claimant is recorded under the slot's own name (KeyOf
// answers a constant), so two plugins claiming the slot collide in the
// startup name resolution -- fatal, naming both: which implementation stands
// must not depend on load order, and bundled plugins load first. Replacing a
// bundled default means disabling it. The first claimant holds the slot
// until then. The core default registers only when no plugin claimed it.
template <typename T, typename KeyOf>
class ExclusiveSlot : public RegistryBase {
public:
    void Register(std::unique_ptr<T> candidate) {
        if (!Admit() || candidate == nullptr) return;
        attributions_.push_back({attribution_, std::string(KeyOf{}(*candidate))});
        if (held_) {
            WarnTaken(holder_, attribution_);
            return;
        }
        held_ = true;
        holder_ = attribution_;
        entry_ = std::move(candidate);
    }

    T* get() const { return entry_.get(); }
    bool held() const { return held_; }
    bool empty() const override { return !held_; }
    // Who holds the slot: a plugin's name, or "" for core.
    const std::string& holder() const { return holder_; }

    // Drops `plugin_name`'s claims, and releases the slot only IF it holds
    // it -- a plugin that failed a LATER hook must not release another's.
    void RemoveByAttribution(const std::string& plugin_name) override {
        std::erase_if(attributions_, [&](const Attribution& attribution) {
            return attribution.plugin_name == plugin_name;
        });
        if (!held_ || holder_ != plugin_name) return;
        entry_.reset();
        held_ = false;
        holder_.clear();
    }

private:
    std::unique_ptr<T> entry_;
    bool held_ = false;
    std::string holder_;
};

struct ViewKey {
    std::string_view operator()(const View& view) const { return view.spec().id; }
};
struct StartViewKey {
    std::string_view operator()(const StartView& view) const { return view.spec().id; }
};
struct ClientCommandKey {
    std::string_view operator()(const ClientCommand& command) const { return command.name(); }
};
struct ToolRendererKey {
    std::string_view operator()(const ToolResultRenderer& r) const { return r.tool_name(); }
};
struct DisplayRendererKey {
    std::string_view operator()(const DisplayRenderer& r) const { return r.kind(); }
};
struct PromptRendererKey {
    std::string_view operator()(const PromptRenderer& r) const { return r.kind(); }
};
struct StatusSegmentKey {
    std::string_view operator()(const StatusSegment& segment) const { return segment.id(); }
};
struct SyntaxGrammarKey {
    std::string_view operator()(const SyntaxGrammar& grammar) const { return grammar.Language(); }
};
// An exclusive slot's key is the slot itself: every claimant collides.
struct ChatSurfaceKey {
    std::string_view operator()(const ChatSurface&) const { return "chat_surface"; }
};
// Observers have no identity to collide on: one constant key.
struct ObserverKey {
    std::string_view operator()(const Observer&) const { return "observer"; }
};
struct MarkdownEngineKey {
    std::string_view operator()(const MarkdownEngine&) const { return "markdown_engine"; }
};
struct WidgetKitKey {
    std::string_view operator()(const WidgetKit&) const { return "widget_kit"; }
};
struct TabBarKey {
    std::string_view operator()(const TabBar&) const { return "tab_bar"; }
};
struct PromptEditorKey {
    std::string_view operator()(const PromptEditorFactory&) const { return "prompt_editor"; }
};

using ViewRegistry = AdditiveRegistry<View, ViewKey>;
using StartViewRegistry = AdditiveRegistry<StartView, StartViewKey>;
using ClientCommandRegistry = AdditiveRegistry<ClientCommand, ClientCommandKey>;
using ToolResultRendererRegistry = AdditiveRegistry<ToolResultRenderer, ToolRendererKey>;
using DisplayRendererRegistry = AdditiveRegistry<DisplayRenderer, DisplayRendererKey>;
using PromptRendererRegistry = AdditiveRegistry<PromptRenderer, PromptRendererKey>;
using StatusSegmentRegistry = AdditiveRegistry<StatusSegment, StatusSegmentKey>;
// The language table the highlighter consults, in registration order.
using SyntaxGrammarRegistry = AdditiveRegistry<SyntaxGrammar, SyntaxGrammarKey>;
using ObserverRegistry = AdditiveRegistry<Observer, ObserverKey>;
// Required: the TUI's body.
using ChatSurfaceRegistry = ExclusiveSlot<ChatSurface, ChatSurfaceKey>;
// Null get(): the host registers the core md4c default.
using MarkdownEngineRegistry = ExclusiveSlot<MarkdownEngine, MarkdownEngineKey>;
// Null get() only during registration: the host registers the core default
// into an unclaimed slot before any view's component() is built.
using WidgetKitRegistry = ExclusiveSlot<WidgetKit, WidgetKitKey>;
// Null get(): no tab bar.
using TabBarRegistry = ExclusiveSlot<TabBar, TabBarKey>;
// Required: the TUI has no other way to take input.
using PromptEditorRegistry = ExclusiveSlot<PromptEditorFactory, PromptEditorKey>;

// Keyed by the ChatRowKind a plugin claims, not by the renderer, so it keeps
// its own Register. First claimant stays active; a second claim is recorded
// (attributions(), keyed by ToString(kind)) and the collision scan turns it
// fatal, like every additive key.
class MessageRendererRegistry : public RegistryBase {
public:
    bool empty() const override { return renderers_.empty(); }
    void Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer);
    // A default for `kind` that any Register() of it replaces (the bundled
    // chat-renderers). Two fallbacks for one kind collide.
    void RegisterFallback(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer);
    // The claim for a kind, else its fallback, else null.
    const MessageRenderer* Find(ChatRowKind kind) const;
    // Whether a claim (not a fallback) answers `kind`: the chat view routes
    // a claimed kind's LIVE rows to it too.
    bool PluginClaimed(ChatRowKind kind) const;

    void RemoveByAttribution(const std::string& plugin_name) override;

private:
    std::map<ChatRowKind, std::unique_ptr<MessageRenderer>> renderers_;
    std::map<ChatRowKind, std::unique_ptr<MessageRenderer>> fallbacks_;
    // Index-aligned with attributions_: the kind each registration named,
    // and whether it was a fallback.
    std::vector<ChatRowKind> kinds_;
    std::vector<bool> fallback_;
};

// --- Chat surface (EXCLUSIVE, REQUIRED) -------------------------------------

// A message queued behind the running turn, until the server takes it.
struct ChatPendingMessage {
    std::uint64_t turn_id = 0;
    std::string text;
};

// One piece of the turn in flight, in arrival order: assistant text, a
// thinking block, or a tool call.
struct ChatLiveSegment {
    enum class Kind { Text, Tool, Thinking };
    Kind kind = Kind::Text;
    // Stable for the segment's life and never reused: key per-segment state
    // (a markdown stream) on it.
    std::uint64_t segment_id = 0;
    // Text and Thinking: everything so far, shared and immutable. Never null.
    std::shared_ptr<const std::string> text = std::make_shared<const std::string>();
    bool text_truncated = false;  // the live text was capped; the transcript has it all
    // Tool: the call -- live, or its final state once the turn ended and the
    // transcript has not caught up yet. Unset: nothing to draw yet.
    std::optional<ToolActivity> tool;
};

// One tab's chat as it stands, built by core each frame from its data plane:
// the transcript window (raw, newest first -- ChatRowsFromMessages
// materializes rows), the turn in flight, the queue. UI thread.
struct ChatFrame {
    std::string session_id;
    // Moves whenever the transcript window changed (per session: key caches
    // on session_id too). rewind_epoch moves when a rewind cut it.
    std::uint64_t transcript_version = 0;
    std::uint64_t rewind_epoch = 0;
    std::shared_ptr<const std::vector<v1::StoredMessage>> scrollback_page;
    std::shared_ptr<const std::vector<v1::CompactionEvent>> compactions;
    bool has_earlier_messages = false;  // older pages exist (chat.load_older)
    bool scrollback_trimmed = false;    // the window evicted its newest pages
    bool scrollback_gap = false;        // a hole between the window and the live turn
    // Effective (what the session runs) and desired (staged, unconfirmed).
    std::string model, agent_preset, desired_model, desired_agent_preset;
    std::vector<ChatPendingMessage> pending_user_messages;
    std::vector<ChatLiveSegment> live_segments;
    // The latest text segment's block just ended: its markdown must be
    // reparsed authoritatively (MarkdownEngine::Stream::Finalize).
    bool streaming_finalized = false;
    bool turn_busy = false;   // thinking, working or waiting
};

// What core gives a chat pane. UI thread.
class ChatHost {
public:
    virtual ~ChatHost() = default;
    // The row renderers (claims and the bundled fallbacks) and the engine.
    virtual const MessageRendererRegistry& MessageRenderers() const = 0;
    virtual const ToolResultRendererRegistry& ToolRenderers() const = 0;
    virtual const DisplayRendererRegistry& DisplayRenderers() const = 0;
    virtual const MarkdownEngine& Markdown() const = 0;
    // The key `action` is bound to ("" when none), for hints in the text.
    virtual std::string KeyFor(std::string_view action) const = 0;
    // Cancels a queued message of tab `tab_id` before it runs.
    virtual void CancelQueued(const std::string& tab_id, std::uint64_t turn_id) = 0;
    // A transient line on tab `tab_id` (feedback for an action: "thinking
    // rows: collapsed (saved)").
    virtual void Notice(const std::string& tab_id, const std::string& text) = 0;
    virtual void RequestRedraw() = 0;
};

// One tab's chat body. Core shows it in the tab's body row and feeds it.
class ChatPane {
public:
    virtual ~ChatPane() = default;
    virtual ftxui::Element Render(const ChatFrame& frame) = 0;
    // One of the surface's declared actions (ChatSurface::Actions), pressed
    // with this tab focused.
    virtual void OnAction(std::string_view action) = 0;
    // A mouse event (wheel, click) with this tab focused. True = consumed
    // (it was over the chat and meant something there).
    virtual bool OnMouse(const ftxui::Event& event) = 0;
    // The user just sent a message on this tab: follow the newest.
    virtual void OnSubmitted() = 0;
};

// The chat view: one pane per tab, made on the tab's first frame. Required:
// the TUI shows nothing without it. A pane that throws shows an error line
// for that tab instead.
class ChatSurface {
public:
    virtual ~ChatSurface() = default;
    virtual std::vector<DeclaredAction> Actions() const = 0;
    // `host` and `theme` outlive the pane.
    virtual std::unique_ptr<ChatPane> Make(ChatHost& host, const std::string& tab_id,
                                           const Theme& theme) = 0;
};

// Every registry the client offers plugins. The host owns one; the loader
// fills it (RegistryHooks in client_plugin_loader.hpp maps each to its
// register_* hook).
struct ClientRegistries {
    ViewRegistry views;
    StartViewRegistry start_views;
    ClientCommandRegistry client_commands;
    ToolResultRendererRegistry tool_renderers;
    DisplayRendererRegistry display_renderers;
    PromptRendererRegistry prompt_renderers;
    MessageRendererRegistry message_renderers;
    StatusSegmentRegistry status_segments;
    SyntaxGrammarRegistry grammars;
    ObserverRegistry observers;
    ChatSurfaceRegistry chat_surface;
    MarkdownEngineRegistry markdown_engine;
    WidgetKitRegistry widget_kit;
    PromptEditorRegistry prompt_editor;
    TabBarRegistry tab_bar;
};

}  // namespace minimal::client::plugins
