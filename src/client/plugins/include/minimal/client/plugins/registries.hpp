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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/subscribe_outcome.hpp"
#include "minimal/plugins/name_table.hpp"
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include <tree_sitter/api.h>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/render/syntax_highlighter.hpp"
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
class HostUi {
public:
    virtual ~HostUi() = default;
    virtual void RequestRedraw() = 0;
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

// Draws one KIND of plugin prompt (docs/EXTENSIBILITY.md, "Plugin prompts")
// as a modal: the client half of a server plugin that calls
// ToolContext::prompt_user. The host shows it in the prompt modal slot (below
// the permission and confirmation modals), rebuilds it per request id, and
// sends whatever `on_reply` receives. A kind with no renderer is answered
// UNAVAILABLE by the host, never left hanging; so is a Make() that throws.
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
class StatusSegment {
public:
    virtual ~StatusSegment() = default;
    virtual std::string_view id() const = 0;
    virtual std::string Render(const std::string& focused_session_id) const = 0;
    // Which segment gives way first when the bar overflows: lowest first
    // (kStatusSegmentPriorities in client/policy.hpp has core's).
    virtual int priority() const { return kPluginStatusSegmentPriority; }
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
    // Canonical language key ("python") -- the fenced-code info string is
    // matched through the core alias table, which consults registered
    // languages too.
    virtual std::string_view Language() const = 0;
    // The tree-sitter parse function itself (tree_sitter_python and friends) --
    // the raw symbol, not a called result, so the highlighter's parser setup
    // treats registered and vendored grammars identically.
    virtual minimal::client::LanguageFnPtr LanguageFn() const = 0;
    virtual std::span<const SyntaxRule> Rules() const = 0;
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

// --- Chat surface (EXCLUSIVE -- the one wholesale-replacement slot) ---------

// One plugin may replace the whole chat view. Exclusive deliberately: the
// interface is big (streaming lifecycle, event push, scrollback, focus) and
// this repo's rule is that an interface which grew that big needs a reason; a
// wholesale chat replacement is that reason, taken once. Message-kind slots
// cover the common case.
class ChatSurface {
public:
    virtual ~ChatSurface() = default;

    struct Spec {
        std::string id;     // stable; how diagnostics and keybindings name it
        std::string title;  // the tab body title, if shown
    };
    virtual const Spec& spec() const = 0;

    // The body of tab `tab_id`. Built per open tab by the host; one
    // component instance per tab. `tab_id` is FocusProvider's TabInfo::tab_id
    // -- the key to that tab's session (Tabs(), or an Observer's events) and
    // so to which of OnEvent's sessions this body shows. The host owns
    // layout: a surface renders INSIDE the tab, it does not restructure the
    // frame.
    virtual ftxui::Component component(const std::string& tab_id) = 0;

    // The host pushes EVERY ServerEvent for the session -- the same events core
    // dispatches to its own state plane. A surface owns its presentation and
    // buffering from there; scrollback pages come through ClientApi. A surface
    // NEVER opens its own connection.
    //
    // THREADING: OnEvent runs on each attached session's gRPC READER thread --
    // one concurrent caller per session, so several may be inside OnEvent at
    // the same time, and none of them is the render thread that calls
    // component()/Render (the host takes no lock across the call -- see
    // AppState::DispatchToTab). It must not block (a wedged reader thread
    // stalls that session's whole data plane) and must be internally
    // synchronized, both against other OnEvent calls and against Render.
    // Events arrive in order PER SESSION; calls for different sessions
    // interleave arbitrarily.
    virtual void OnEvent(const v1::ServerEvent& event) = 0;

    // Called once after the theme is loaded (plugins register before it). The
    // surface re-reads colors through it; Render paths receive the theme the
    // same way core views do.
    virtual void OnTheme(const Theme& theme) = 0;
};

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
    // `positional`: whitespace-split arguments; `raw`: everything after the
    // name, untouched.
    virtual void Run(const std::vector<std::string>& positional, const std::string& raw) = 0;
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
template <typename T, typename KeyOf>
class AdditiveRegistry : public RegistryBase {
public:
    // Takes ownership. Null, or an empty key, registers nothing.
    void Register(std::unique_ptr<T> entry) {
        if (!Admit() || entry == nullptr) return;
        std::string key(KeyOf{}(*entry));
        if (key.empty()) return;
        surfaces_.push_back(key);
        attributions_.push_back({attribution_, std::move(key)});
        entries_.push_back(std::move(entry));
    }

    // In registration order.
    const std::vector<std::unique_ptr<T>>& All() const { return entries_; }
    // Index-aligned with All(): the name each entry answers to.
    const std::vector<std::string>& surfaces() const { return surfaces_; }

    T* Find(std::string_view surface) const {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (surfaces_[i] == surface) return entries_[i].get();
        }
        return nullptr;
    }

    void ApplyAliases(const std::map<std::string, std::string>& aliases) override {
        for (std::size_t i = 0; i < attributions_.size(); ++i) {
            const auto it = aliases.find(minimal::plugins::StableIdentity(
                attributions_[i].plugin_name, attributions_[i].original_name));
            if (it != aliases.end()) surfaces_[i] = it->second;
        }
    }

    void RemoveByAttribution(const std::string& plugin_name) override {
        // entries_, surfaces_ and attributions_ are index-aligned: compact
        // them together.
        std::size_t write = 0;
        for (std::size_t read = 0; read < attributions_.size(); ++read) {
            if (attributions_[read].plugin_name == plugin_name) continue;
            if (write != read) {
                attributions_[write] = std::move(attributions_[read]);
                entries_[write] = std::move(entries_[read]);
                surfaces_[write] = std::move(surfaces_[read]);
            }
            ++write;
        }
        attributions_.resize(write);
        entries_.resize(write);
        surfaces_.resize(write);
    }

private:
    std::vector<std::unique_ptr<T>> entries_;
    std::vector<std::string> surfaces_;
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
// Null get(): core's chat view stands.
using ChatSurfaceRegistry = ExclusiveSlot<ChatSurface, ChatSurfaceKey>;
// Null get(): the host registers the core md4c default.
using MarkdownEngineRegistry = ExclusiveSlot<MarkdownEngine, MarkdownEngineKey>;

// Keyed by the ChatRowKind a plugin claims, not by the renderer, so it keeps
// its own Register. First claimant stays active; a second claim is recorded
// (attributions(), keyed by ToString(kind)) and the collision scan turns it
// fatal, like every additive key.
class MessageRendererRegistry : public RegistryBase {
public:
    void Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer);
    // The renderer for a kind, or null -- the caller falls back to the core
    // default (which is itself registered here once core fills it).
    const MessageRenderer* Find(ChatRowKind kind) const;
    // Whether the ACTIVE renderer for `kind` is a plugin's (the first claim),
    // not core's default fill.
    bool PluginClaimed(ChatRowKind kind) const;

    void RemoveByAttribution(const std::string& plugin_name) override;

private:
    std::map<ChatRowKind, std::unique_ptr<MessageRenderer>> renderers_;
    // Index-aligned with attributions_: the kind each claim named.
    std::vector<ChatRowKind> kinds_;
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
};

}  // namespace minimal::client::plugins
