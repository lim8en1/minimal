#pragma once

// What the host gives a client plugin (docs/EXTENSIBILITY.md 4.2): the UI
// seam (frames, view control, reads), the action verbs, the attention sink,
// the sanctioned data path (ClientApi), focus and the typed host events.
// Each arrives through ClientPluginRegistrationContext; a null seam means the
// host does not offer it, which a plugin treats as degraded content, never
// an error.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/client/sdk/command_catalog_entry.hpp"
#include "minimal/client/sdk/subscribe_outcome.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client::plugins {

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

// One client plugin, as the TUI has it (HostUi::ClientPlugins): what
// /config lists and toggles.
struct ClientPluginInfo {
    std::string id;                // its file: what SetClientPluginEnabled names
    std::string name;              // its own name; the file's stem when it never loaded
    bool enabled = false;          // on for this run
    bool enabled_in_file = false;  // client-plugins.json now: the next start's
    bool loaded = false;
    std::string error;             // why it failed to load, when it did
    // What it registered, by registry label ("views": ["config"]).
    std::map<std::string, std::vector<std::string>> provides;
    // Why the TUI cannot start without it ("draws the chat"); "" = optional.
    std::string required;
};

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
    // Every client plugin the TUI found, loaded or not. (The default: none.)
    virtual std::vector<ClientPluginInfo> ClientPlugins() const { return {}; }
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

    // Turns client plugin `id` (ClientPluginInfo::id) on or off in
    // client-plugins.json, for the next TUI start; the focused tab's notice
    // says what happened. Refused for a plugin the TUI cannot start without
    // (ClientPluginInfo::required).
    virtual void SetClientPluginEnabled(const std::string& id, bool enabled) = 0;
};

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

}  // namespace minimal::client::plugins
