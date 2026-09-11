#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/view/tab_ui.hpp"

#include "minimal/common/logger.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

bool ModalSlotStale(const std::string& tracked_request_id, const std::string& request_id,
                    const std::string& slot_tab, const std::string& focused_tab) {
    return tracked_request_id != request_id || slot_tab != focused_tab;
}

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
SessionState::Snapshot TabMaintenance::Maintain(const std::string& tab_id,
                                                SessionState::Snapshot snap) {
    // Gated SOLELY on EffectiveChatSurface(): it is what actually renders the
    // tab body (FrameRenderer::Render reads the same accessor), and it turns
    // null the moment a registered surface throws and
    // AppState::DisableChatSurface runs -- the frame falls back to the core
    // ChatView right then. A prior revision also checked a by-value copy of
    // the registered surface pointer (constant since bootstrap), which made
    // this return early forever once a surface was ever registered, even
    // after DisableChatSurface flipped the effective accessor back to null:
    // the core view the frame fell back to never got fed a streaming delta
    // or a scrollback refresh, so the fallback rendered nothing, permanently.
    if (deps_.app.EffectiveChatSurface() != nullptr) return snap;
    // A sessionless tab (no session yet) has nothing streamed or persisted:
    // no ChatView, no streaming bookkeeping, no refresh. Its SessionState
    // gains an id the moment EnsureSession resolves, and the next pass runs.
    if (snap.session_id.empty()) return snap;
    TabUi& ui = deps_.tab_ui[tab_id];

    // The chat view itself: constructed once per tab, lazily -- a tab that
    // never leaves the start-view banner (a fresh session with nothing
    // streamed or persisted yet) may never need one, but building it here
    // rather than in Render() means Render() is a pure read of ui.chat for
    // every tab this method has processed at least once (Render() keeps its
    // own null-guarded fallback for the one documented race where it hasn't
    // -- see frame_renderer.cpp).
    if (!ui.chat) {
        ui.chat = std::make_unique<ChatView>(deps_.theme, deps_.message_registry,
                                             deps_.renderer_registry, deps_.display_registry);
    }

    const auto make_stream = [&] { return deps_.engine.MakeStream(); };

    // One stream slot per live_segments entry (track C4), index-aligned;
    // recreated WHOLESALE on turn_seq (SessionState's counter, bumped when a
    // new streaming turn begins) -- same trigger the old single shared
    // `streaming` field used, NOT the old buffer-size heuristic: turn A
    // ending and turn B streaming past A's byte count between two frames
    // spliced B's middle onto A's cached tail, silently. Segments only ever
    // APPEND within a turn (AppendStreamingText/OnToolStarted push, nothing
    // removes mid-turn), so growing the vector to match -- without clearing
    // slots already built for EARLIER segments -- is exactly what a new
    // segment arriving needs.
    if (ui.streaming_turn != snap.turn_seq) {
        ui.live_text_streams.clear();
        ui.streaming_turn = snap.turn_seq;
    }
    if (ui.live_text_streams.size() < snap.live_segments.size()) {
        ui.live_text_streams.resize(snap.live_segments.size());
    }
    for (std::size_t i = 0; i < snap.live_segments.size(); ++i) {
        if (snap.live_segments[i].kind != LiveTurnSegment::Kind::Text) continue;
        LiveTextStreamSlot& slot = ui.live_text_streams[i];
        const std::string& text = snap.live_segments[i].text;
        // A plugin MarkdownEngine is not trusted to return normally: a
        // throw here used to kill the whole TUI mid-turn. MakeStream()
        // throwing leaves slot.view null -- the guard above naturally
        // retries next pass, since nothing below has consumed `text` yet.
        if (!slot.view) {
            try {
                slot.view = make_stream();
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown engine MakeStream threw: {}",
                                             error.what());
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown engine MakeStream threw a non-standard exception");
            }
        }
        if (slot.view && text.size() > slot.seen) {
            try {
                slot.view->AppendDelta(std::string_view(text).substr(slot.seen));
                slot.seen = text.size();
                // New text on an ALREADY-finalized segment means a re-arm at
                // the engine level (StreamingMarkdownView::AppendDelta, track
                // A1) -- mirror that here too, so this segment gets its own
                // fresh authoritative reparse when it next finalizes, exactly
                // like the engine's own re-arm.
                slot.finalized_for_turn = false;
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown stream AppendDelta threw: {}",
                                             error.what());
                // Discard the (possibly now-corrupted) stream rather than
                // keep feeding it every frame; slot.seen resets too, so the
                // fresh stream MakeStream() builds next pass replays the
                // FULL accumulated text from scratch instead of missing the
                // span this call was trying to append.
                slot.view.reset();
                slot.seen = 0;
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown stream AppendDelta threw a non-standard exception");
                slot.view.reset();
                slot.seen = 0;
            }
        }
    }
    // The MOST RECENT text segment's authoritative reparse (the engine
    // contract: Finalize replaces artifacts from mid-stream unclosed spans).
    // is_streaming_finalized fires once per BLOCK boundary, and blocks are
    // strictly sequential (never interleaved with each other -- only with
    // tool activity, which is what live_segments itself now records), so it
    // always names the LAST text segment in the timeline; an earlier one
    // already got its own Finalize when ITS block ended.
    if (snap.is_streaming_finalized) {
        for (std::size_t i = snap.live_segments.size(); i-- > 0;) {
            if (snap.live_segments[i].kind != LiveTurnSegment::Kind::Text) continue;
            LiveTextStreamSlot& slot = ui.live_text_streams[i];
            if (slot.view && !slot.finalized_for_turn) {
                try {
                    slot.view->Finalize();
                    slot.finalized_for_turn = true;
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("markdown stream Finalize threw: {}",
                                                 error.what());
                    slot.view.reset();
                    slot.seen = 0;
                } catch (...) {
                    common::DefaultLogger().warn(
                        "markdown stream Finalize threw a non-standard exception");
                    slot.view.reset();
                    slot.seen = 0;
                }
            }
            break;
        }
    }

    // Resolve any outstanding refresh and act on the version mismatch --
    // the whole decision (submit / latch / mark-current, success vs
    // failure, streaming vs turn boundary) is AdvanceScrollbackRefresh,
    // unit-tested in chat_model_test.cpp. This call either submits the
    // newest-slice fetch or leaves the bookkeeping current; the fetch
    // itself is async and its application wakes the pump for a later
    // frame.
    if (AdvanceScrollbackRefresh(ui.refresh, snap) == ScrollbackRefreshAction::kSubmit) {
        deps_.app.RequestScrollback(snap.session_id, /*newest=*/true);
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
    } else if (notice.view_hint == "models") {
        deps_.open_model_picker();   // core mapping: bare `/model`
    } else if (auto* view = deps_.view_registry.Find(notice.view_hint)) {
        for (auto& entry : deps_.plugin_views) {
            if (entry.view != view) continue;
            // Same focus-follows-open rule as the keybinding toggle (see
            // keybinding_dispatch.hpp's focus-rule contract).
            const bool was_visible = entry.visible;
            entry.visible = true;
            if (!was_visible) FocusPanelOnOpen(deps_.plugin_views, entry);
        }
        if (view->spec().placement == plugins::ViewPlacement::Modal) {
            // REUSE the staged component (bootstrap built one instance per
            // view; the keybinding path stages that same instance). Calling
            // component() again here built a SECOND instance with its own
            // internal state, so the modal shown via a notice was a
            // different object than the one the keybinding toggles. The
            // call is still plugin code and still contained: if the entry
            // has no component (bootstrap's build threw), keep the modal
            // closed rather than crash.
            for (auto& entry : deps_.plugin_views) {
                if (entry.view != view) continue;
                if (entry.component) {
                    *deps_.plugin_modal_child = entry.component;
                    deps_.show_plugin_modal = true;
                } else {
                    common::DefaultLogger().warn(
                        "plugin view '{}' has no component; modal not opened",
                        std::string(view->spec().id));
                    *deps_.plugin_modal_child = nullptr;
                }
                break;
            }
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
        if (ModalSlotStale(ui.grants_for_request, request.request_id, permission_slot_tab_,
                           focused_)) {
            ui.grants_for_request = request.request_id;
            permission_slot_tab_ = focused_;
            // ONE shared_ptr shared by the modal component and the answer
            // closure. The old code captured a raw `&toggles` (pointee of
            // ui.grants) in the closure, which stayed valid only through the
            // implicit invariant that ui.grants is never reset while its
            // modal lives -- and the modal's checkboxes bind raw bool* into
            // the toggles deque, so the component and the closure must agree
            // on the SAME object anyway. Ownership inside the modal/closure
            // pair makes the lifetime explicit: the toggles live exactly as
            // long as the component that displays and answers them, even if
            // the next request replaces this modal wholesale.
            const auto toggles = std::make_shared<GrantToggles>();
            // AnswerPermission is SESSION-keyed; a modal can only exist on a
            // sessioned tab, so the snapshot's session id is the right key.
            const auto session_id = snap.session_id;
            AppState& app = deps_.app;
            *deps_.permission_child = MakePermissionModal(
                request, *toggles, deps_.theme,
                [&app, session_id, request, toggles](PermissionVerdict verdict) {
                    // granted_keys FILTERED to what this request offered
                    // (GrantToggles reads the request's own offers). Grants
                    // ride only an allow; the message and the stop only a deny.
                    const bool allow = verdict.choice == PermissionChoice::Allow;
                    app.AnswerPermission(session_id, request.request_id,
                                         allow ? v1::PERMISSION_DECISION_ALLOW
                                               : v1::PERMISSION_DECISION_DENY,
                                         allow ? toggles->GrantedKeys(request)
                                               : std::vector<std::string>{},
                                         verdict.choice == PermissionChoice::DenyWithMessage
                                             ? std::move(verdict.message)
                                             : std::string{},
                                         verdict.choice == PermissionChoice::DenyAndStop);
                });
        }
        deps_.show_permission = true;
    } else {
        deps_.show_permission = false;
    }

    // The wire confirmation modal: same rebuild-on-new-request-id rule as
    // the permission modal above. All three pending modals are never set at
    // once (each Set clears the others in SessionState).
    if (snap.pending_confirmation.has_value()) {
        const auto& request = *snap.pending_confirmation;
        if (ModalSlotStale(ui.confirmation_for_request, request.request_id,
                           confirmation_slot_tab_, focused_)) {
            ui.confirmation_for_request = request.request_id;
            confirmation_slot_tab_ = focused_;
            const auto session_id = snap.session_id;   // SESSION-keyed (see above)
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

    // The model's structured ask (tool-ask plugin): same rule again. The
    // modal builds the whole AskUserResponse itself (it owns the per-question
    // UI state the answers come from); Escape hands back an answered=false
    // response, which AnswerAsk sends unchanged.
    if (snap.pending_ask.has_value()) {
        const auto& request = *snap.pending_ask;
        if (ModalSlotStale(ui.ask_for_request, request.request_id, ask_slot_tab_, focused_)) {
            ui.ask_for_request = request.request_id;
            ask_slot_tab_ = focused_;
            const auto session_id = snap.session_id;
            const auto request_copy = request;
            AppState& app = deps_.app;
            *deps_.ask_child = MakeAskModal(request_copy, deps_.theme,
                                            [&app, session_id](v1::AskUserResponse response) {
                                                app.AnswerAsk(session_id, response);
                                            });
        }
        deps_.show_ask = true;
    } else {
        deps_.show_ask = false;
    }
}

void TabMaintenance::Run() {
    const uint64_t version = deps_.app.UiVersion();
    if (version == last_ui_version_) return;
    last_ui_version_ = version;

    // A picker's async fetch completion is exactly what bumped the version
    // that got this pass past the gate above (PickerController::QueueApply
    // calls AppState::NotifyExternalUpdate, the same BumpUiVersion every
    // ordinary mutation goes through) -- draining it here, unconditionally,
    // is what turns that wake into an actual rebuild of the picker's modal.
    if (deps_.poll_pickers) deps_.poll_pickers();

    const auto tabs = deps_.app.GetAllTabs();
    focused_.clear();
    for (const auto& tab : tabs) {
        if (tab.is_focused) focused_ = tab.tab_id;
    }
    if (focused_.empty()) {
        focused_details_ = SessionState::Snapshot{};
        deps_.show_permission = false;
        deps_.show_confirmation = false;
        deps_.show_ask = false;
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
