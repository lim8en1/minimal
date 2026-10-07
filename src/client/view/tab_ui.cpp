#include "minimal/client/keybinding_dispatch.hpp"
#include "minimal/client/view/tab_ui.hpp"

#include <algorithm>
#include <exception>
#include <functional>
#include <string_view>

#include <nlohmann/json.hpp>

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
void TabMaintenance::EnsureChatPane(const std::string& tab_id, TabUi& ui) {
    if (ui.chat || !deps_.make_chat_pane) return;
    ui.chat = deps_.make_chat_pane(tab_id);
}

SessionState::Snapshot TabMaintenance::Maintain(const std::string& tab_id,
                                                SessionState::Snapshot snap) {
    // A sessionless tab (no session yet) has nothing streamed or persisted:
    // no ChatView, no streaming bookkeeping, no refresh. Its SessionState
    // gains an id the moment EnsureSession resolves, and the next pass runs.
    if (snap.session_id.empty()) return snap;
    TabUi& ui = deps_.tab_ui[tab_id];

    // The chat pane: built once per tab, lazily -- here rather than in
    // Render(), so Render() only reads ui.chat for every tab this method has
    // processed (it keeps a fallback for the one race where it hasn't).
    EnsureChatPane(tab_id, ui);

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
    if (auto* view = deps_.view_registry.Find(notice.view_hint)) {
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

nlohmann::json PermissionPayload(const PendingPermission& request) {
    nlohmann::json grants = nlohmann::json::array();
    for (const auto& grant : request.grants) {
        grants.push_back({{"key", grant.key()}, {"label", grant.label()}});
    }
    return {{"tool_name", request.tool_name},
            {"category", v1::ToolCategory_Name(request.category)},
            {"summary", request.summary},
            {"complete", request.complete},
            {"reason", request.reason},
            {"grants", std::move(grants)}};
}

nlohmann::json ConfirmationPayload(const std::string& kind, const std::string& title,
                                   const std::string& detail,
                                   const std::vector<std::string>& options) {
    return {{"kind", kind}, {"title", title}, {"detail", detail}, {"options", options}};
}

void AnswerPermissionFromReply(AppState& app, const std::string& session_id,
                               const PendingPermission& request, const plugins::PromptReply& reply) {
    bool allow = false;
    bool stop = false;
    std::vector<std::string> granted;
    std::string message;
    if (reply.outcome == v1::PROMPT_OUTCOME_ANSWERED && reply.answer.is_object()) {
        const std::string decision = reply.answer.value("decision", "");
        if (decision == "allow") {
            allow = true;
            // Only what this request OFFERED: an answer can never mint a
            // grant for a key no prompt displayed (the server filters too).
            for (const auto& key : reply.answer.value("granted_keys", nlohmann::json::array())) {
                if (!key.is_string()) continue;
                for (const auto& offer : request.grants) {
                    if (offer.key() == key.get<std::string>()) granted.push_back(offer.key());
                }
            }
        } else if (decision == "deny_with_message") {
            message = reply.answer.value("message", "");  // empty: a plain deny
        } else if (decision == "deny_and_stop") {
            stop = true;
        }
        // "deny", and anything else: deny.
    }
    app.AnswerPermission(session_id, request.request_id,
                         allow ? v1::PERMISSION_DECISION_ALLOW : v1::PERMISSION_DECISION_DENY,
                         std::move(granted), std::move(message), stop);
}

std::string ConfirmationChoice(const std::vector<std::string>& options,
                               const plugins::PromptReply& reply) {
    // One of the offered options, or "" -- the server's own "do not proceed".
    if (reply.outcome != v1::PROMPT_OUTCOME_ANSWERED || !reply.answer.is_object()) return {};
    const std::string chosen = reply.answer.value("chosen", "");
    for (const auto& option : options) {
        if (option == chosen) return chosen;
    }
    return {};
}

ftxui::Component MakeConsentModal(const plugins::PromptRendererRegistry& registry,
                                  std::string_view kind, const nlohmann::json& payload,
                                  const std::string& asker, const Theme& theme,
                                  std::function<void(plugins::PromptReply)> reply) {
    const auto* renderer = registry.Find(kind);
    if (renderer == nullptr) {
        common::DefaultLogger().error("no prompt renderer for core's '{}' kind: answering no", kind);
        return nullptr;
    }
    try {
        return renderer->Make(plugins::PromptView{.kind = kind, .payload = payload, .asker = asker},
                              theme, std::move(reply));
    } catch (const std::exception& error) {
        common::DefaultLogger().error("prompt renderer for '{}' threw: {} -- answering no", kind,
                                      error.what());
    } catch (...) {
        common::DefaultLogger().error("prompt renderer for '{}' threw -- answering no", kind);
    }
    return nullptr;
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

    // Core's two questions, drawn by whichever plugin renders the
    // "permission" / "confirmation" prompt kinds (both required at startup).
    // Core builds the payload, keeps the layer topmost, checks the answer and
    // fails closed: no modal, a throw, or anything but a well-formed choice
    // answers deny / "do not proceed" at once -- the server worker is parked
    // on the request and nothing else would answer it.
    //
    // Each modal is keyed on its request id ALONE, never on the component:
    // a renderer that throws builds none, and with the answer's send failing
    // too (the request stays pending) a null-component key re-built, re-threw
    // and re-answered on every pass. The key clears when the request does,
    // so the same id arriving again (a replay after reattach) is drawn.
    if (snap.pending_permission.has_value()) {
        const auto& request = *snap.pending_permission;
        if (ui.grants_for_request != request.request_id) {
            ui.grants_for_request = request.request_id;
            // AnswerPermission is SESSION-keyed; a modal can only exist on a
            // sessioned tab, so the snapshot's session id is the right key.
            const auto session_id = snap.session_id;
            AppState& app = deps_.app;
            const auto answer = [&app, session_id, request](const plugins::PromptReply& reply) {
                AnswerPermissionFromReply(app, session_id, request, reply);
            };
            ui.permission_modal = MakeConsentModal(deps_.prompt_registry, "permission",
                                                   PermissionPayload(request), snap.agent_preset,
                                                   deps_.theme, answer);
            if (ui.permission_modal == nullptr) answer({v1::PROMPT_OUTCOME_UNAVAILABLE, nullptr});
        }
        *deps_.permission_child = ui.permission_modal;
        deps_.show_permission = ui.permission_modal != nullptr;
    } else {
        ui.permission_modal = nullptr;
        ui.grants_for_request.clear();
        deps_.show_permission = false;
    }

    // The wire confirmation: the same rule.
    if (snap.pending_confirmation.has_value()) {
        const auto& request = *snap.pending_confirmation;
        if (ui.confirmation_for_request != request.request_id) {
            ui.confirmation_for_request = request.request_id;
            const auto session_id = snap.session_id;   // SESSION-keyed (see above)
            AppState& app = deps_.app;
            const auto answer = [&app, session_id, request](const plugins::PromptReply& reply) {
                app.AnswerConfirmation(session_id, request.request_id,
                                       ConfirmationChoice(request.options, reply));
            };
            ui.confirmation_modal =
                MakeConsentModal(deps_.prompt_registry, "confirmation",
                                 ConfirmationPayload(request.kind, request.title, request.detail,
                                                     request.options),
                                 snap.agent_preset, deps_.theme, answer);
            if (ui.confirmation_modal == nullptr) answer({v1::PROMPT_OUTCOME_UNAVAILABLE, nullptr});
        }
        *deps_.confirmation_child = ui.confirmation_modal;
        deps_.show_confirmation = ui.confirmation_modal != nullptr;
    } else {
        ui.confirmation_modal = nullptr;
        ui.confirmation_for_request.clear();
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
        ui.prompt_for_request.clear();
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
