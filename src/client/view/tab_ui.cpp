#include "minimal/client/view/tab_ui.hpp"

#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

TabMaintenance::TabMaintenance(Dependencies deps) : deps_(std::move(deps)) {}

// Per-frame maintenance for the focused tab: feed streaming deltas to the
// debounced view, and trigger the post-turn scrollback refresh. A turn in
// flight is NOT fetched around -- the streaming block is the live content
// until the turn ends and the refresh swaps it for the persisted rows.
// (Skipped entirely when the chat surface owns the tab body: the surface
// keeps its own buffers and fetches its own pages.)
//
// T2: the refresh itself is async (AppState::RequestScrollback enqueues it
// and returns immediately), so this function cannot re-fetch and observe
// the result within the same call -- it returns the snapshot UNCHANGED, and
// the worker's completion (BumpUiVersion) wakes the pump for a frame that
// will see the applied page in a freshly-taken snapshot.
SessionState::Snapshot TabMaintenance::Maintain(const std::string& session_id,
                                                SessionState::Snapshot snap) {
    if (deps_.chat_surface != nullptr || deps_.app.EffectiveChatSurface() != nullptr) return snap;
    if (snap.session_id.empty() && session_id != snap.session_id) return snap;
    TabUi& ui = deps_.tab_ui[session_id];

    // The chat view itself: constructed once per tab, lazily -- a tab that
    // never leaves the start-view banner (a fresh session with nothing
    // streamed or persisted yet) may never need one, but building it here
    // rather than in Render() means Render() is a pure read of ui.chat for
    // every tab this method has processed at least once (Render() keeps its
    // own null-guarded fallback for the one documented race where it hasn't
    // -- see frame_renderer.cpp).
    if (!ui.chat) {
        ui.chat = std::make_unique<ChatView>(deps_.theme, deps_.message_registry,
                                             deps_.renderer_registry);
    }

    const auto make_stream = [&] { return deps_.engine.MakeStream(); };

    // Stream recreation keys on turn_seq (SessionState's counter, bumped
    // when a new streaming turn begins), NOT on the old buffer-size
    // heuristic: turn A ending and turn B streaming past A's byte count
    // between two frames spliced B's middle onto A's cached tail, silently.
    if (ui.streaming == nullptr || ui.streaming_turn != snap.turn_seq) {
        ui.streaming = make_stream();
        ui.streaming_seen = 0;
        ui.streaming_turn = snap.turn_seq;
        ui.finalized_for_turn = false;
    } else if (snap.streaming_text.size() > ui.streaming_seen) {
        ui.streaming->AppendDelta(std::string_view(snap.streaming_text).substr(ui.streaming_seen));
        ui.streaming_seen = snap.streaming_text.size();
    }

    // The turn's authoritative reparse (the engine contract: Finalize
    // replaces artifacts from mid-stream unclosed spans). Without this
    // wiring the contract was dead code -- the last tokens of a stream
    // rendered only when the debounce's next tick happened to fire.
    if (snap.is_streaming_finalized && !ui.finalized_for_turn && ui.streaming) {
        ui.streaming->Finalize();
        ui.finalized_for_turn = true;
    }

    // Resolve any outstanding refresh and act on the version mismatch --
    // the whole decision (submit / latch / mark-current, success vs
    // failure, streaming vs turn boundary) is AdvanceScrollbackRefresh,
    // unit-tested in chat_model_test.cpp. This call either submits the
    // newest-slice fetch or leaves the bookkeeping current; the fetch
    // itself is async and its application wakes the pump for a later
    // frame.
    if (AdvanceScrollbackRefresh(ui.refresh, snap) == ScrollbackRefreshAction::kSubmit) {
        deps_.app.RequestScrollback(session_id, /*newest=*/true);
    }
    return snap;
}

// VIEW-display notice side-effects (open the picker, show a plugin modal)
// fire ONCE per notice here, keyed on the notice's sequence number -- never
// from Render(). Routing this inside the renderer let the notice (never
// cleared -- status text persists until the next result replaces it)
// re-run open_picker's ListSessions RPC and re-stage the modal at frame
// rate, making it unclosable. Status/Silent notices keep rendering in the
// frame; only their SIDE-EFFECTS are guarded here.
void TabMaintenance::HandleNotice(const SessionState::Snapshot& snap) {
    if (focused_.empty()) return;
    if (!snap.latest_notice.has_value()) return;
    TabUi& ui = deps_.tab_ui[focused_];
    if (ui.handled_notice_seq == snap.notice_seq) return;   // already handled
    ui.handled_notice_seq = snap.notice_seq;

    const auto& notice = *snap.latest_notice;
    if (notice.display != v1::COMMAND_DISPLAY_VIEW || notice.view_hint.empty()) {
        return;
    }
    if (notice.view_hint == "sessions") {
        deps_.open_picker();   // core mapping: the picker modal
    } else if (notice.view_hint == "agents") {
        deps_.open_agent_picker();   // core mapping: bare `/agent`
    } else if (auto* view = deps_.view_registry.Find(notice.view_hint)) {
        for (auto& entry : deps_.plugin_views) {
            if (entry.view == view) entry.visible = true;
        }
        if (view->spec().placement == plugins::ViewPlacement::Modal) {
            *deps_.plugin_modal_child = view->component();
            deps_.show_plugin_modal = true;
        }
    }
    // A hint naming no view falls through: the unknown-view-id rule, like
    // the unknown segment id's.
}

// The permission modal for the focused tab: rebuilt when the outstanding
// request changes, shown while it is unanswered. Moved here from
// FrameRenderer::Render() (Part 3.2): deciding "is this a NEW request" and
// rebuilding a modal component are event-path decisions, and Render() must
// stay a pure read -- see frame_renderer.hpp's file comment for the
// property this restores. Both blocks below are verbatim what Render() used
// to do, just relocated.
void TabMaintenance::MaintainModals(const SessionState::Snapshot& snap) {
    TabUi& ui = deps_.tab_ui[focused_];

    if (snap.pending_permission.has_value()) {
        const auto& request = *snap.pending_permission;
        if (ui.grants_for_request != request.request_id) {
            ui.grants = std::make_unique<GrantToggles>();
            ui.grants_for_request = request.request_id;
            const auto session_id = focused_;
            GrantToggles& toggles = *ui.grants;
            AppState& app = deps_.app;
            *deps_.permission_child = MakePermissionModal(
                request, toggles, deps_.theme,
                [&app, session_id, request, &toggles](bool allow) {
                    // granted_keys FILTERED to what this request offered
                    // (GrantToggles reads the request's own offers).
                    app.AnswerPermission(session_id, request.request_id,
                                         allow ? v1::PERMISSION_DECISION_ALLOW
                                               : v1::PERMISSION_DECISION_DENY,
                                         allow ? toggles.GrantedKeys(request)
                                               : std::vector<std::string>{});
                });
        }
        deps_.show_permission = true;
    } else {
        deps_.show_permission = false;
    }

    // The wire confirmation modal: same rebuild-on-new-request-id rule as
    // the permission modal above. Both are never set at once (each Set
    // clears the other in SessionState).
    if (snap.pending_confirmation.has_value()) {
        const auto& request = *snap.pending_confirmation;
        if (ui.confirmation_for_request != request.request_id) {
            ui.confirmation_for_request = request.request_id;
            const auto session_id = focused_;
            const auto request_copy = request;
            AppState& app = deps_.app;
            *deps_.confirmation_child = MakeConfirmationModal(
                request_copy, deps_.theme,
                [&app, session_id, request_copy](const std::string& chosen) {
                    app.AnswerConfirmation(session_id, request_copy.request_id, chosen);
                });
        }
        deps_.show_confirmation = true;
    } else {
        deps_.show_confirmation = false;
    }
}

void TabMaintenance::Run() {
    const uint64_t version = deps_.app.UiVersion();
    if (version == last_ui_version_) return;
    last_ui_version_ = version;

    const auto tabs = deps_.app.GetAllTabs();
    focused_.clear();
    for (const auto& tab : tabs) {
        if (tab.is_focused) focused_ = tab.session_id;
    }
    if (focused_.empty()) {
        focused_details_ = SessionState::Snapshot{};
        deps_.show_permission = false;
        deps_.show_confirmation = false;
        return;
    }
    // ONE deep snapshot per pass: GetDetails is a full protobuf copy of the
    // scrollback window (up to scrollback_window_max_messages rows), and
    // this used to be paid up to three times per frame. Maintain() may
    // replace it with a fresher one if a scrollback refresh fires (it must
    // -- the refresh mutates SessionState out from under the copy this call
    // started with); HandleNotice and the renderer both read whatever
    // Maintain() settled on.
    focused_details_ = Maintain(focused_, deps_.app.GetDetails(focused_));
    HandleNotice(focused_details_);
    MaintainModals(focused_details_);
}

}  // namespace minimal::client
