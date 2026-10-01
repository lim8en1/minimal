#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/view/tab_ui.hpp"

#include <algorithm>

#include "minimal/common/logger.hpp"
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
void TabMaintenance::EnsureChatView(const std::string& tab_id, TabUi& ui) {
    if (ui.chat) return;
    // The keymap's hints, not the ChatView defaults: a remapped
    // chat.toggle_collapse must reach the row hints.
    ui.chat = std::make_unique<ChatView>(deps_.theme, deps_.message_registry,
                                         deps_.renderer_registry, deps_.display_registry,
                                         deps_.toggle_collapse_hint, deps_.load_older_hint);
    ui.chat->SetFoldDefaults(deps_.fold_thinking_default, deps_.fold_tool_default);
    // Click-to-cancel for QUEUED message rows: the view is per-tab, so the
    // handler binds this tab's own id.
    if (deps_.cancel_pending_turn) {
        ui.chat->SetCancelPendingHandler([this, tab_id](std::uint64_t turn_id) {
            deps_.cancel_pending_turn(tab_id, turn_id);
        });
    }
}

void TabMaintenance::SetFoldDefaults(std::optional<int> thinking, std::optional<int> tool) {
    if (thinking) deps_.fold_thinking_default = *thinking;
    if (tool) deps_.fold_tool_default = *tool;
}

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
    EnsureChatView(tab_id, ui);

    const auto make_stream = [&] { return deps_.engine.MakeStream(); };

    // One stream slot per live TEXT segment (track C4), keyed by the
    // segment's stable id (LiveTurnSegment::segment_id) -- NOT its vector
    // index. The old index-aligned vector assumed "segments only ever APPEND
    // within a turn", but three paths REMOVE without bumping turn_seq:
    // LiveTurnBuffer::Retire (a mid-turn page apply), the count cap's
    // trim, and ClearStreamingIfCurrentTurn. The survivors shift down, an
    // index-keyed slot's `seen` watermark then named a DIFFERENT segment,
    // and AppendDelta(text.substr(seen)) spliced the tail of one segment
    // onto another segment's buffered view -- corrupted for the rest of the
    // turn ("random unrelated text" at the end of long thinking blocks;
    // self-healing only because the next turn's turn_seq bump cleared the
    // whole vector). Ids are never reused, and removals are permanent
    // within a turn, so a slot whose id is absent from this pass's snapshot
    // is dead -- the GC below retires it, which also replaces the old
    // wholesale turn-boundary clear.
    std::vector<uint64_t> live_text_ids;
    live_text_ids.reserve(snap.live_segments.size());
    for (const auto& segment : snap.live_segments) {
        if (segment.kind != LiveTurnSegment::Kind::Text) continue;
        live_text_ids.push_back(segment.segment_id);
        LiveTextStreamSlot* slot = nullptr;
        for (auto& candidate : ui.live_text_streams) {
            if (candidate.segment_id == segment.segment_id) {
                slot = &candidate;
                break;
            }
        }
        if (slot == nullptr) {
            ui.live_text_streams.push_back(
                LiveTextStreamSlot{.segment_id = segment.segment_id});
            slot = &ui.live_text_streams.back();
        }
        // The snapshot's copy-on-publish view (state.hpp LiveTurnSegment) --
        // shared with every other consumer of this segment's current text;
        // only the actively-appending segment republishes per delta.
        const std::string& text = segment.Text();
        // A plugin MarkdownEngine is not trusted to return normally: a
        // throw here used to kill the whole TUI mid-turn. MakeStream()
        // throwing leaves slot.view null -- the guard above naturally
        // retries next pass, since nothing below has consumed `text` yet.
        if (!slot->view) {
            try {
                slot->view = make_stream();
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown engine MakeStream threw: {}",
                                             error.what());
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown engine MakeStream threw a non-standard exception");
            }
        }
        if (slot->view && text.size() > slot->seen) {
            try {
                slot->view->AppendDelta(std::string_view(text).substr(slot->seen));
                slot->seen = text.size();
                // New text on an ALREADY-finalized segment means a re-arm at
                // the engine level (StreamingMarkdownView::AppendDelta, track
                // A1) -- mirror that here too, so this segment gets its own
                // fresh authoritative reparse when it next finalizes, exactly
                // like the engine's own re-arm.
                slot->finalized_for_turn = false;
            } catch (const std::exception& error) {
                common::DefaultLogger().warn("markdown stream AppendDelta threw: {}",
                                             error.what());
                // Discard the (possibly now-corrupted) stream rather than
                // keep feeding it every frame; slot.seen resets too, so the
                // fresh stream MakeStream() builds next pass replays the
                // FULL accumulated text from scratch instead of missing the
                // span this call was trying to append.
                slot->view.reset();
                slot->seen = 0;
            } catch (...) {
                common::DefaultLogger().warn(
                    "markdown stream AppendDelta threw a non-standard exception");
                slot->view.reset();
                slot->seen = 0;
            }
        }
    }
    // GC: slots whose segment is gone. Safe against a transient absence
    // because segments never reappear: every removal path (retirement,
    // trim, turn-aware clear) is permanent, and ids are never reused.
    ui.live_text_streams.erase(
        std::remove_if(ui.live_text_streams.begin(), ui.live_text_streams.end(),
                       [&](const LiveTextStreamSlot& slot) {
                           return std::find(live_text_ids.begin(), live_text_ids.end(),
                                            slot.segment_id) == live_text_ids.end();
                       }),
        ui.live_text_streams.end());
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
            LiveTextStreamSlot* slot = nullptr;
            for (auto& candidate : ui.live_text_streams) {
                if (candidate.segment_id == snap.live_segments[i].segment_id) {
                    slot = &candidate;
                    break;
                }
            }
            if (slot != nullptr && slot->view && !slot->finalized_for_turn) {
                try {
                    slot->view->Finalize();
                    slot->finalized_for_turn = true;
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("markdown stream Finalize threw: {}",
                                                 error.what());
                    slot->view.reset();
                    slot->seen = 0;
                } catch (...) {
                    common::DefaultLogger().warn(
                        "markdown stream Finalize threw a non-standard exception");
                    slot->view.reset();
                    slot->seen = 0;
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
    // A mismatch the backoff is holding: remember when it may retry, so the
    // pump wakes a frame for it (RefreshRetryDue) and Run() lets that frame
    // through its UiVersion gate.
    const bool held = !ui.refresh.refresh_pending &&
                      ui.refresh.fetched_generation != snap.fetch_generation;
    refresh_retry_at_.store(
        held ? ui.refresh.next_retry_earliest.time_since_epoch().count() : 0,
        std::memory_order_release);
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
    } else if (notice.view_hint == "subagent-models") {
        deps_.open_subagent_model_picker();   // core mapping: bare `/subagent-model`
    } else if (notice.view_hint == "subagents") {
        // core mapping: `/subagents` -- the picker lists the FOCUSED
        // session's own children (a read-only command, so it fires just as
        // well from a nested subagent's tab, whose children these are).
        if (!snap.session_id.empty()) deps_.open_subagents_picker(snap.session_id);
    } else if (auto* view = deps_.view_registry.Find(notice.view_hint)) {
        // The keybinding's own opener (keybinding_dispatch.hpp): the same
        // staged component instance, the same focus rule -- and a modal
        // retires any other modal's `visible`, not only a panel's focus.
        for (auto& entry : deps_.plugin_views) {
            if (entry.view != view) continue;
            ShowPluginView(deps_.plugin_views, entry, *deps_.plugin_modal_child,
                           deps_.show_plugin_modal);
            break;
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
        if (ui.permission_modal == nullptr || ui.grants_for_request != request.request_id) {
            ui.grants_for_request = request.request_id;
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
            ui.permission_modal = MakePermissionModal(
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
        *deps_.permission_child = ui.permission_modal;
        deps_.show_permission = true;
    } else {
        ui.permission_modal = nullptr;
        deps_.show_permission = false;
    }

    // The wire confirmation modal: same rebuild-on-new-request-id rule as
    // the permission modal above. All three pending modals are never set at
    // once (each Set clears the others in SessionState).
    if (snap.pending_confirmation.has_value()) {
        const auto& request = *snap.pending_confirmation;
        if (ui.confirmation_modal == nullptr ||
            ui.confirmation_for_request != request.request_id) {
            ui.confirmation_for_request = request.request_id;
            const auto session_id = snap.session_id;   // SESSION-keyed (see above)
            const auto request_copy = request;
            AppState& app = deps_.app;
            ui.confirmation_modal = MakeConfirmationModal(
                request_copy, deps_.theme,
                [&app, session_id, request_copy](const std::string& chosen) {
                    app.AnswerConfirmation(session_id, request_copy.request_id, chosen);
                });
        }
        *deps_.confirmation_child = ui.confirmation_modal;
        deps_.show_confirmation = true;
    } else {
        ui.confirmation_modal = nullptr;
        deps_.show_confirmation = false;
    }

    // A plugin prompt: same rule again, drawn by the client plugin that
    // registered its kind (the renderer owns the answer's whole shape; core
    // only relays the outcome and the JSON). No renderer for the kind, or a
    // renderer that throws, is answered UNAVAILABLE at once -- the server
    // worker is parked on this prompt and nothing else would ever answer it.
    if (snap.pending_prompt.has_value()) {
        const auto& request = *snap.pending_prompt;
        // Keyed on the request id alone (not on the component): a kind no
        // renderer draws is answered UNAVAILABLE once, not on every pass.
        if (ui.prompt_for_request != request.request_id) {
            ui.prompt_for_request = request.request_id;
            const auto session_id = snap.session_id;
            const auto request_id = request.request_id;
            AppState& app = deps_.app;
            const auto reply = [&app, session_id, request_id](plugins::PromptReply answer) {
                app.AnswerPluginPrompt(
                    session_id, request_id, answer.outcome,
                    answer.outcome == v1::PROMPT_OUTCOME_ANSWERED
                        ? answer.answer.dump(-1, ' ', false,
                                             nlohmann::json::error_handler_t::replace)
                        : std::string{});
            };
            ftxui::Component modal;
            if (const auto* renderer = deps_.prompt_registry.Find(request.kind)) {
                try {
                    modal = renderer->Make(
                        plugins::PromptView{.kind = request.kind,
                                            .payload = request.payload,
                                            .asker = snap.agent_preset},
                        deps_.theme, reply);
                } catch (const std::exception& error) {
                    common::DefaultLogger().warn("prompt renderer for kind '{}' threw: {}",
                                                 request.kind, error.what());
                } catch (...) {
                    common::DefaultLogger().warn(
                        "prompt renderer for kind '{}' threw a non-standard exception",
                        request.kind);
                }
            } else {
                common::DefaultLogger().warn(
                    "no prompt renderer for kind '{}': answering unavailable", request.kind);
            }
            if (modal == nullptr) {
                reply(plugins::PromptReply{v1::PROMPT_OUTCOME_UNAVAILABLE, nullptr});
            }
            ui.prompt_modal = std::move(modal);
        }
        *deps_.prompt_child = ui.prompt_modal;
        deps_.show_prompt = ui.prompt_modal != nullptr;
    } else {
        ui.prompt_modal = nullptr;
        deps_.show_prompt = false;
    }
}

bool TabMaintenance::RefreshRetryDue(std::chrono::steady_clock::time_point now) const {
    const int64_t at = refresh_retry_at_.load(std::memory_order_acquire);
    return at != 0 && now.time_since_epoch().count() >= at;
}

void TabMaintenance::Run() {
    const uint64_t version = deps_.app.UiVersion();
    if (version == last_ui_version_ && !RefreshRetryDue()) return;
    last_ui_version_ = version;
    refresh_retry_at_.store(0, std::memory_order_release);   // Maintain() re-arms it

    // A picker's async fetch completion is exactly what bumped the version
    // that got this pass past the gate above (PickerController::QueueApply
    // calls AppState::NotifyExternalUpdate, the same BumpUiVersion every
    // ordinary mutation goes through) -- draining it here, unconditionally,
    // is what turns that wake into an actual rebuild of the picker's modal.
    if (deps_.drain_loop_work) deps_.drain_loop_work();

    const auto tabs = deps_.app.GetAllTabs();
    focused_.clear();
    for (const auto& tab : tabs) {
        if (tab.is_focused) focused_ = tab.tab_id;
    }
    if (focused_.empty()) {
        focused_details_ = SessionState::Snapshot{};
        deps_.show_permission = false;
        deps_.show_confirmation = false;
        deps_.show_prompt = false;
        return;
    }
    // A notice's self-expiry: one tick per pass THIS gate already let through,
    // i.e. once per focused-tab-visible UI change -- not per repaint, since a
    // repaint with nothing new does not reach here at all. Before GetDetails()
    // below, so a notice that expires this pass is already gone from the
    // snapshot HandleNotice/the renderer see, instead of lingering one extra
    // frame.
    deps_.app.TickTabNotice(focused_);
    // ONE deep snapshot per pass: GetDetails copies the live segments, the
    // tools and the pending prompts (the scrollback page itself is shared,
    // copy-on-apply), and this used to be paid up to three times per frame. Maintain() may
    // replace it with a fresher one if a scrollback refresh fires (it must
    // -- the refresh mutates SessionState out from under the copy this call
    // started with); HandleNotice and the renderer both read whatever
    // Maintain() settled on.
    focused_details_ = Maintain(focused_, deps_.app.GetDetails(focused_));
    HandleNotice(focused_details_);
    MaintainModals(focused_details_);
}

}  // namespace minimal::client
