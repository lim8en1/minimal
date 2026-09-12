#pragma once

// The session picker and agent preset picker's data + fetch/rebuild/open
// flows, factored out of RunTui (app.cpp). Both pickers follow the same
// shape RunTui's own comments already named: "modal over data it does not
// own" -- picker_data/agent_picker_presets are the fetched list, a mutating
// action (resume/delete/new) closes the modal, and toggling all-dirs or
// loading more re-fetches and rebuilds the component in place. Delete is
// the exception on the return trip: its confirmation hides the picker for
// its duration, and resolving it restores the picker (RefreshSessionPicker
// re-fetches so the deleted row is gone).
//
// PickerController owns that state privately (nothing outside this class
// read picker_data or agent_picker_presets in the inline code either) and
// writes the rebuilt component into the SAME slot Modal() reads from in
// RunTui's frame assembly -- `picker_slot`/`agent_picker_slot` are
// references to shared_ptr<Component> locals RunTui still owns and passes
// to MakeModalHost itself; this class only ever assigns *through* them
// (`*picker_slot = ...`), the same as the inline closures did.
//
// ASYNC (ARCHITECTURE.md review, Part 4): every list/refresh RPC
// (ListSessions, ListAgentPresets, ListModels -- the last up to 15 s with
// refresh_discovery) runs on AppState's own RPC worker via RunOnRpcQueue,
// never the FTXUI loop thread. picker_data_/agent_picker_presets_/
// model_picker_models_ and the modal Component slots are still
// loop-thread-owned -- unchanged from the synchronous version -- so a
// completion never touches them directly from the worker thread: it packages
// plain fetched data into a closure and hands it to QueueApply, which stashes
// it under async_mtx_ and wakes the pump (AppState::NotifyExternalUpdate).
// Poll(), called from TabMaintenance::Run() on the event path, drains that
// queue and runs each closure here on the loop thread -- that is the only
// place picker_data_ or a modal slot is written from a fetch's result.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/session_client.hpp"
#include "minimal/client/state.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

class PickerController {
public:
    struct Dependencies {
        SessionClient& client;
        AppState& app;
        const Theme& theme;

        // Modal slots: `*picker_slot = MakeSessionPicker(...)`, mirroring
        // every other "modal over a shared_ptr<Component> slot" in RunTui
        // (permission_child, confirmation_child, ...).
        std::shared_ptr<ftxui::Component>& picker_slot;
        std::shared_ptr<ftxui::Component>& agent_picker_slot;
        std::shared_ptr<ftxui::Component>& model_picker_slot;
        std::shared_ptr<ftxui::Component>& mcp_picker_slot;

        bool& show_picker;
        bool& show_agent_picker;
        bool& show_model_picker;
        bool& show_mcp_picker;

        // The session picker's delete flow stages its OWN confirmation
        // layer (styled like the quit one) rather than deleting inline.
        std::string& staged_delete_id;
        bool& show_delete_confirm;

        // T8: the picker's "+ new session" row reaches the same OpenTab
        // path typing on an empty app does. Left as a callback (not a
        // method here) because it belongs to session-lifecycle code this
        // class has no other reason to know about.
        std::function<void()> open_new_session;

        // The desired-state pickers (/agent, /model) stage onto the FOCUSED
        // tab -- which may not EXIST: a bare /agent typed on the empty
        // startup app opens this picker with zero tabs, and staging onto
        // FocusedTabId() then silently dropped both selections, so the
        // first message created the session on the configured default
        // instead. Same action typing takes: the focused tab, or a fresh
        // sessionless one (OpenTab focuses an empty app on its first tab).
        // Callback for the same reason open_new_session is.
        std::function<std::string()> focused_or_new_tab;
    };

    explicit PickerController(Dependencies deps);

    // The picker-open flow, shared by the keybinding and by the /sessions
    // view_hint. T8: always a fresh first page in the project's own
    // directory.
    void OpenSessionPicker();

    // Shared by the keybinding and by the bare `/agent` view_hint. One
    // fresh fetch each time: the preset list is small, no pagination state
    // to preserve across opens.
    void OpenAgentPicker();

    // Shared by the bare `/model` view_hint and the picker's own ctrl+r.
    // The bare open lists what the server already knows (static models plus
    // any cached discovery); `refresh` re-fetches every discover-entry's
    // catalog first, which can take seconds -- runs on AppState's RPC worker,
    // never the loop thread (see the file comment).
    void OpenModelPicker(bool refresh);

    // The /mcp picker's list: every mcps.json entry's live status. One fresh
    // fetch each time, like OpenAgentPicker -- the list is small.
    void OpenMcpPicker();

    // Drains completed async fetches and rebuilds whichever picker they
    // belong to. Call every pass, unconditionally -- cheap (one lock, an
    // empty check) when nothing is pending. See TabMaintenance::Run.
    void Poll();

    // Re-fetches the session list from scratch and rebuilds the picker in
    // place -- RunTui calls this when the delete confirmation resolves with
    // "delete", so the removed row drops out of the still-open picker. The
    // reset keeps include_all_dirs (only the fetched rows are cleared); the
    // rebuild, like every rebuild, starts a fresh filter and cursor.
    void RefreshSessionPicker();

private:
    void FetchPickerPage(bool reset);
    void RebuildPickerComponent();
    void RebuildAgentPickerComponent();
    void RebuildModelPickerComponent();
    void RebuildMcpPickerComponent();
    // Enter on a picker row: flips the server's enabled flag, then re-fetches
    // and rebuilds -- the round trip on_refresh_discovery uses for models,
    // minus the discovery-refresh option (there is nothing else to refresh
    // here beyond the toggle itself).
    void ToggleMcpServer(const std::string& name);

    // Queues `apply` to run on the LOOP THREAD from the next Poll(), and
    // wakes the pump so that Poll() actually gets called. The caller (an
    // RpcQueue job) must not have touched picker_data_/agent_picker_presets_/
    // model_picker_models_ or a modal slot itself -- `apply` is where that
    // happens, and it is one of those two things (never a bare RPC result).
    void QueueApply(std::function<void()> apply);

    // The picker's open action: ATTACH for a live row, RESUME otherwise --
    // see the definition for why the split is load-bearing. `is_live` is
    // resolved by the caller on the LOOP THREAD (a scan of picker_data_,
    // loop-thread-owned) before this runs on the RPC worker -- reading
    // picker_data_ from here instead would race Poll()'s own writes to it.
    std::string OpenPickedSession(const std::string& session_id, bool is_live);

    // T8: the picker's raw accumulated list, in server delivery order
    // (updated_at DESC, id ASC per session_service.proto). Filter/sort are
    // pure DISPLAY transforms MakeSessionPicker applies fresh -- this stays
    // in fetch order so "load more"'s cursor always derives from the last
    // element the SERVER actually sent, never from whatever the current
    // sort happens to show last.
    struct PickerData {
        std::vector<v1::SessionSummary> sessions;
        bool include_all_dirs = false;
        bool has_more = false;   // the last fetch returned a full page
    };

    Dependencies deps_;
    PickerData picker_data_;
    // "Load more" in-flight latch (loop-thread state, like picker_data_):
    // FetchPickerPage(false) refuses to queue a second same-cursor fetch
    // while one is outstanding, and the fetch's apply closure (or its
    // failure path) releases it. Without it, key-repeat on arrow-down-at-end
    // queued several identical ListSessions calls whose arrivals each
    // appended the SAME page -- duplicated rows -- and each arrival rebuilt
    // the picker component, resetting its filter/selection per rebuild.
    // Alongside picker_fetch_generation_ below, not instead of it: this
    // latch only ever stops a SECOND same-cursor call from being SENT; it
    // says nothing about whether an already-sent fetch's result is still
    // wanted by the time it lands.
    bool picker_fetch_in_flight_ = false;
    // Fetch supersession counter (loop-thread state, like the fields above):
    // bumped every time FetchPickerPage actually sends a request (both
    // reset and load-more -- reset is exempt from picker_fetch_in_flight_
    // above, so it can start a second RunOnRpcQueue job while an older one
    // is still outstanding, e.g. a delete confirmed while a load-more is in
    // flight). Each RunOnRpcQueue job captures its own generation by value;
    // its QueueApply closure(s) compare that captured value against this
    // field's CURRENT value before touching anything -- a mismatch means a
    // later fetch has already superseded this one (the reset already
    // cleared and re-paginated picker_data_), so the closure no-ops rather
    // than appending stale rows or releasing a latch a newer fetch now owns.
    uint64_t picker_fetch_generation_ = 0;
    // The agent picker's list: fetched whole on each open (no pagination,
    // no sort -- agents/*.md is small and has no "recent" axis).
    std::vector<v1::AgentPresetSummary> agent_picker_presets_;
    // The model picker's list: fetched whole on each open (refs are sorted
    // server-side; grouping is a pure display transform). No pagination --
    // even a large discovered catalog is one response of small rows.
    std::vector<v1::ModelSummary> model_picker_models_;
    // The /mcp picker's list: fetched whole on each open/toggle, like the
    // model list -- a server list is small, no pagination.
    std::vector<v1::McpServerSummary> mcp_picker_servers_;

    // The async completion handoff (see the file comment): a fetch running
    // on AppState's RPC worker pushes its apply-closure here under the lock
    // and wakes the pump; Poll() (loop thread) swaps the vector out under the
    // same lock and runs each closure OUTSIDE it -- an apply closure that
    // itself queued another fetch (OpenModelPicker's ctrl+r, say) must not
    // dead-lock re-entering QueueApply while Poll() still holds it.
    std::mutex async_mtx_;
    std::vector<std::function<void()>> pending_apply_;
};

}  // namespace minimal::client
