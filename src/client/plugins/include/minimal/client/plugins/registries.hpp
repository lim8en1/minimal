#pragma once

// The client slot registries (docs/EXTENSIBILITY.md 4.2) -- the TUI's pluggable
// presentation surface, mirroring the server's registry pattern:
//
//   - ADDITIVE registries (views, tool-result renderers, message renderers,
//     status segments, syntax grammars) take many plugins; a duplicate key is
//     recorded through the shared NameRegistry and is FATAL at startup, naming
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

#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/subscribe_outcome.hpp"
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>
#include <tree_sitter/api.h>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/render/syntax_highlighter.hpp"
#include "minimal/v1/session_service.pb.h"

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
class AttentionSink {
public:
    virtual ~AttentionSink() = default;
    virtual void RaiseTab(const std::string& session_id) = 0;
    virtual void ClearTab(const std::string& session_id) = 0;
    virtual void RaiseGlobal() = 0;
    virtual void ClearGlobal() = 0;
};

// The host-side state behind AttentionSink, shared by the sink and AppState.
// Created before plugin load; AppState reads it per frame. Thread-safe: Raise
// arrives from plugin code (often a plugin's data thread), reads happen on the
// render thread.
class AttentionState {
public:
    void RaiseTab(const std::string& session_id);
    void ClearTab(const std::string& session_id);
    void RaiseGlobal();
    void ClearGlobal();

    // A plugin raise wins until cleared; core derivation ORs in underneath.
    bool TabRaised(const std::string& session_id) const;
    bool GlobalRaised() const;
    std::vector<std::string> RaisedTabs() const;

private:
    mutable std::mutex mtx_;
    std::map<std::string, bool> tab_;
    bool global_ = false;
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
    // The click affordance for collapsible rows: " [+]" while collapsed,
    // " [-]" while expanded, "" for kinds a click never collapses. Appended
    // to the row's HEADER line; a renderer for a collapsible kind that
    // ignores it simply forgoes the visual hint (clicking still works).
    std::string_view collapse_marker = "";
};

// --- Tool-result renderers (keyed by tool name) -----------------------------

// A renderer for one tool's RESULT row -- "a diff view for `edit`", per
// ARCHITECTURE.md. Rendering a CORE tool is as legitimate as a plugin tool:
// keyed by name, source-agnostic. A renderer for a tool whose plugin is
// disabled (or for a name nothing registers) simply never matches -- the
// fallback is the default collapsible row, not an error.
class ToolResultRenderer {
public:
    virtual ~ToolResultRenderer() = default;
    virtual std::string_view tool_name() const = 0;
    // Takes the tool INPUT as well as its result -- a diff renderer needs the
    // file path, not just the hunk text (ARCHITECTURE.md). `tool_input` is the
    // JSON the model sent with the call; empty object when unavailable.
    virtual ftxui::Element Render(const nlohmann::json& tool_input, std::string_view result_text,
                                  bool is_error, const Theme& theme,
                                  const RowRenderContext& ctx) const = 0;
};

// --- Message-kind presentation (keyed by ChatRowKind) -----------------------

// Renderers for one kind of chat row; core supplies the defaults the
// message-kind table specifies. The DATA PLANE (event demux, virtualization,
// streaming buffer) stays core regardless of who renders a row. A plugin
// claiming a kind replaces the core default for that kind only; a second
// plugin claiming the same kind is a fatal collision (a new NameRegistry kind
// is overkill for an enum key -- the registry tracks (kind, plugin) pairs and
// the loader surfaces duplicates through the same fatal path).
class MessageRenderer {
public:
    virtual ~MessageRenderer() = default;
    virtual ftxui::Element Render(const ChatRow& row, const Theme& theme,
                                  const RowRenderContext& ctx) const = 0;
};

// --- Status-bar segments (keyed by segment id) ------------------------------

// One provider per segment id; the format string (ui.json `status_format`)
// orders and selects by id, so a plugin adds content while the user keeps
// layout control. An unknown id renders nothing with a startup warning --
// the keybindings.json rule for names unknown at compile time.
//
// Render() is called on the render thread each frame (~20 Hz): it must be
// cheap and must never block -- a segment that needs live data (a plugin RPC,
// say) fetches it on its OWN background thread and has Render() read a cached
// answer, never call out synchronously from here. Color comes from theme
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
};

// --- Syntax grammars (additive, keyed by language) --------------------------

// One language grammar: the vendored set registers as the defaults, and a
// plugin may add a language (a grammar ships its own TSLanguage* -- the
// vendored symbols are not exported). Two plugins claiming the same language
// is a fatal collision (a NameRegistry kind). The rule type is the
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

    // The tab body. Built per open tab by the host; one component instance per
    // tab. The host owns layout: a surface renders INSIDE the tab, it does not
    // restructure the frame.
    virtual ftxui::Component component() = 0;

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
    std::string id;  // stable; how keybindings.json addresses it
    std::string title;
    ViewPlacement placement = ViewPlacement::Panel;
    // Registered as the DEFAULT binding only if the user's keybindings.json
    // does not already name this id (Keybindings::EnsureDefault).
    std::string default_keybinding;
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
};

// --- Start views (additive: the initial view on start) ----------------------

// The initial view rendered while there is no conversation content yet: the
// app-level empty state (no tab open) and a freshly opened session before its
// first message arrives. Additive -- every registered start view renders,
// stacked in registration order; a duplicate id is fatal through the shared
// NameRegistry like every other view id.
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

// --- Registries -------------------------------------------------------------

// Registration-time-only enforcement, the server's Seal ported: after the
// loader has run every plugin's register_* hook, the host seals each
// registry. A plugin that stashed its registry reference and registered
// LATER used to bypass the collision rule, the attribution windows and (for
// exclusive slots) the core default entirely, silently; a sealed Register
// warns and is ignored instead.
class RegistrationWindow {
public:
    void Seal() { sealed_ = true; }

protected:
    bool sealed_ = false;
};

// Common attribution mechanics, identical to the server's registries: the
// loader opens a window around each plugin's registration callback and
// everything arriving inside it is attributed to that plugin.
struct Attribution {
    std::string plugin_name;    // empty for what core registers itself
    std::string original_name;  // the key as registered
};

class ViewRegistry : public RegistrationWindow {
public:
    // Registry owns the view. Returns the attribution recorded; a duplicate id
    // is NOT silently replaced -- it is recorded for the fatal-collision scan
    // and the first registration stays active.
    void Register(std::unique_ptr<View> view);
    const std::vector<std::unique_ptr<View>>& views() const { return views_; }
    View* Find(std::string_view id) const;

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    const std::vector<Attribution>& attributions() const { return attributions_; }

private:
    std::vector<std::unique_ptr<View>> views_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

class StartViewRegistry : public RegistrationWindow {
public:
    void Register(std::unique_ptr<StartView> view);
    const std::vector<std::unique_ptr<StartView>>& views() const { return views_; }
    StartView* Find(std::string_view id) const;

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    const std::vector<Attribution>& attributions() const { return attributions_; }

private:
    std::vector<std::unique_ptr<StartView>> views_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

class ToolResultRendererRegistry : public RegistrationWindow {
public:
    void Register(std::unique_ptr<ToolResultRenderer> renderer);
    const ToolResultRenderer* Find(std::string_view tool_name) const;
    const std::vector<Attribution>& attributions() const { return attributions_; }

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

private:
    std::vector<std::unique_ptr<ToolResultRenderer>> renderers_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

class MessageRendererRegistry : public RegistrationWindow {
public:
    // Claims `kind`. The FIRST claimant for a kind stays; a second claim (from
    // another plugin) is recorded -- the loader turns it into the same fatal
    // outcome a name collision gets.
    void Register(ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer);
    // The renderer for a kind, or null -- the caller falls back to the core
    // default (which is itself registered here once core fills it).
    const MessageRenderer* Find(ChatRowKind kind) const;
    // (kind, plugin) pairs, for the collision scan.
    const std::vector<std::pair<ChatRowKind, Attribution>>& claims() const { return claims_; }

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

private:
    std::map<ChatRowKind, std::unique_ptr<MessageRenderer>> renderers_;
    std::string attribution_;
    std::vector<std::pair<ChatRowKind, Attribution>> claims_;
};

class StatusSegmentRegistry : public RegistrationWindow {
public:
    void Register(std::unique_ptr<StatusSegment> segment);
    const std::vector<std::unique_ptr<StatusSegment>>& segments() const { return segments_; }
    const StatusSegment* Find(std::string_view id) const;
    const std::vector<Attribution>& attributions() const { return attributions_; }

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

private:
    std::vector<std::unique_ptr<StatusSegment>> segments_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

class SyntaxGrammarRegistry : public RegistrationWindow {
public:
    void Register(std::unique_ptr<SyntaxGrammar> grammar);
    // The language table the highlighter consults, in registration order.
    const std::vector<std::unique_ptr<SyntaxGrammar>>& grammars() const { return grammars_; }
    const std::vector<Attribution>& attributions() const { return attributions_; }

    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

private:
    std::vector<std::unique_ptr<SyntaxGrammar>> grammars_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

// Exclusive registries: first registration wins, the second warns and is
// ignored. Kept as records rather than lookup tables -- there is exactly one
// active registrant, or none.
struct ExclusiveRecord {
    bool registered = false;
    std::string plugin_name;  // who holds the slot; empty for the core default

    // False (and warns) when the slot was already taken. Never overwrites.
    bool Claim(std::string_view claimant);
};

class ChatSurfaceRegistry : public RegistrationWindow {
public:
    std::unique_ptr<ChatSurface> surface;  // null when core's chat view stands
    ExclusiveRecord record;
    const std::vector<Attribution>& attributions() const { return attributions_; }
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    // Takes ownership; warns and ignores when the slot is already claimed.
    void Register(std::unique_ptr<ChatSurface> surface);

private:
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

class MarkdownEngineRegistry : public RegistrationWindow {
public:
    std::unique_ptr<MarkdownEngine> engine;  // null: host uses the core default
    ExclusiveRecord record;
    const std::vector<Attribution>& attributions() const { return attributions_; }
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }
    void Register(std::unique_ptr<MarkdownEngine> engine);

private:
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

}  // namespace minimal::client::plugins
