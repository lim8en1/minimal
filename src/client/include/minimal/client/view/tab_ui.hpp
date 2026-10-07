#pragma once

// Per-tab UI caches and the frame's maintenance pass, factored out of
// RunTui (app.cpp). TabUi is genuinely UI-plumbing data (a streaming view,
// grant-checkbox state, a ChatView instance) that RunTui's still-inline
// render body indexes directly by session id -- it stays a plain
// std::map<std::string, TabUi> RunTui owns, the same as before; only the
// LOGIC that maintains it (TabMaintenance) moves into a class here.
//
// TabMaintenance does NOT own `tab_ui`: it takes it by reference in
// Dependencies, the same pattern KeyContext and PickerController::
// Dependencies already use for everything else RunTui owns. This matters
// for declaration ORDER, not just style -- close_tab (RunTui, defined
// before TabMaintenance can be constructed: it needs open_picker/
// plugin_modal_child, which are built later) erases from `tab_ui` directly,
// so the map itself must exist as an ordinary RunTui local from the point
// TabUi's type is known, not appear only once every dependency of the
// maintenance logic is available.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>

#include <nlohmann/json.hpp>

#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"
#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/scrollback_refresh.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/ui_config.hpp"

namespace minimal::client {

// Core's side of the consent prompts ("permission", "confirmation"): plugin
// renderers draw them (consent-prompts, bundled; both kinds required at
// startup), core builds the payload and reads the answer, failing closed.
//
// The renderer registered for `kind`, built over `payload`; nullptr (logged)
// when none is registered or it throws -- the caller answers "no" at once.
ftxui::Component MakeConsentModal(const plugins::PromptRendererRegistry& registry,
                                  std::string_view kind, const nlohmann::json& payload,
                                  const std::string& asker, const Theme& theme,
                                  std::function<void(plugins::PromptReply)> reply);
nlohmann::json PermissionPayload(const PendingPermission& request);
nlohmann::json ConfirmationPayload(const std::string& kind, const std::string& title,
                                   const std::string& detail,
                                   const std::vector<std::string>& options);
// Answers `request` from a "permission" reply: allow only on an ANSWERED
// {"decision":"allow"}, granting only offered keys; deny otherwise.
void AnswerPermissionFromReply(AppState& app, const std::string& session_id,
                               const PendingPermission& request, const plugins::PromptReply& reply);
// A "confirmation" reply's choice: one of `options`, or "" (do not proceed).
std::string ConfirmationChoice(const std::vector<std::string>& options,
                               const plugins::PromptReply& reply);

struct TabUi {
    // The permission modal's rebuild key: rebuilt only when a NEW
    // request_id arrives (its grant toggles live in the renderer's modal).
    std::string grants_for_request;
    // The wire confirmation modal's rebuild key, like grants_for_request:
    // the inner modal is rebuilt only when a NEW request_id arrives.
    std::string confirmation_for_request;
    // A plugin prompt modal's rebuild key, same rule.
    std::string prompt_for_request;
    // THIS tab's built modals, kept per tab: the app's modal slots are
    // shared, and focus A -> B -> A used to rebuild A's modal from scratch
    // -- a half-typed deny message or partial prompt answers were lost on
    // every tab switch. The shared slot is pointed at the focused tab's own
    // component instead.
    ftxui::Component permission_modal;
    ftxui::Component confirmation_modal;
    ftxui::Component prompt_modal;
    // The tab's chat pane (the chat surface's, one per tab): a view's row
    // cache keyed by a per-session transcript_version served tab A's rows as
    // tab B's whenever the versions collided -- one pane per tab makes that
    // impossible by construction. Null until the tab first needs one.
    std::unique_ptr<plugins::ChatPane> chat;
    // VIEW-display notice side-effects fire ONCE per notice, keyed on the
    // notice's sequence number (see TabMaintenance::HandleNotice).
    uint64_t handled_notice_seq = 0;
    // Each slot's own finalized_for_turn fires that segment's one
    // authoritative reparse (see TabMaintenance::Maintain); slots live and
    // die with their segment ids, so no turn-boundary wholesale recreation
    // is needed.
    // The newest-slice refresh state machine (T2, reworked): submitted on a
    // version mismatch, resolved via Snapshot::refresh_seq, with the
    // submit/latch/mark-current decision in AdvanceScrollbackRefresh
    // (chat_model.hpp) -- the pure seam where the "user prompt seen only on
    // the first message" bug is now unit-tested.
    ScrollbackRefreshState refresh;
};

// THE MAINTENANCE PASS: everything the frame needs but must not do from
// Render -- a fresh focused-tab snapshot, the per-tab streaming/refresh
// bookkeeping, and the once-per-notice VIEW side-effects (a View-display
// CommandResult may stage a modal and run a bounded RPC; doing that inside
// the renderer made the frame stall and the staged modal unclosable). Run()
// is meant to be called from the event path (RunTui's root CatchEvent),
// never from a Renderer.
class TabMaintenance {
public:
    struct Dependencies {
        AppState& app;
        plugins::ViewRegistry& view_registry;
        std::vector<plugins::PluginViewEntry>& plugin_views;
        std::shared_ptr<ftxui::Component>& plugin_modal_child;
        bool& show_plugin_modal;
        // Drains PickerController's async fetch completions and rebuilds
        // whichever picker's component they belong to (PickerController::Poll).
        // Called every pass BEFORE the version-gate below can skip it: a
        // completion's own NotifyExternalUpdate() is what bumps UiVersion()
        // and gets Run() past that gate in the first place, so placing this
        // call anywhere else would either miss it or need its own separate
        // wake path. A picker fetch that runs on AppState's RPC worker
        // (session/agent/model lists, previously synchronous RPCs up to 15 s
        // on the loop thread -- ARCHITECTURE.md review) must never touch its
        // modal's Component slot from that thread; this is the loop-thread
        // side of that handoff.
        // (It also applies plugins' queued HostUi view requests: the same
        // "loop-thread side of a cross-thread handoff" shape.)
        std::function<void()> drain_loop_work;
        // NOT owned -- see the file comment on declaration ordering.
        std::map<std::string, TabUi>& tab_ui;

        // Part 3.2: the ChatView construction, and the permission/
        // confirmation modal rebuild-on-new-request logic, both moved here
        // from FrameRenderer::Render() -- see tab_ui.cpp's MaintainModals
        // for why (Render() must stay a pure read; deciding "is this a NEW
        // request" and rebuilding a modal component are event-path
        // decisions, not render-path ones).
        const Theme& theme;
        // Plugin prompts are drawn by the renderer registered for their kind;
        // an unclaimed kind is answered UNAVAILABLE (see MaintainModals).
        const plugins::PromptRendererRegistry& prompt_registry;
        std::shared_ptr<ftxui::Component>& permission_child;
        bool& show_permission;
        std::shared_ptr<ftxui::Component>& confirmation_child;
        bool& show_confirmation;
        std::shared_ptr<ftxui::Component>& prompt_child;
        bool& show_prompt;
        // Makes tab `tab_id`'s chat pane (the chat surface's Make, contained:
        // null when it threw).
        std::function<std::unique_ptr<plugins::ChatPane>(const std::string& tab_id)> make_chat_pane;
    };

    explicit TabMaintenance(Dependencies deps);

    // Gated on AppState::UiVersion(): every AppState mutation bumps it at
    // its own entry point, so "unchanged since the last pass" is a sound
    // proxy for "nothing Maintain()/HandleNotice() would read has changed
    // either" -- skips the light GetAllTabs() plus the full deep
    // GetDetails() protobuf copy otherwise paid on every event, including
    // every keystroke and mouse move that never touches AppState at all.
    void Run();

    // What the last successful pass settled on: the focused tab's TAB id
    // (AppState's tab handle -- valid for sessionless tabs too). References
    // into this object's own members -- valid as long as this TabMaintenance
    // is (RunTui declares one plain local and never relocates it, same
    // requirement as PickerController/KeyContext's dependents).
    // THE chat pane builder for a tab (no-op when `ui` already has one): the
    // maintenance pass calls it for the focused tab, and FrameRenderer for
    // the race where a frame renders a tab this pass has not processed yet.
    void EnsureChatPane(const std::string& tab_id, TabUi& ui);

    const std::string& Focused() const { return focused_; }
    const SessionState::Snapshot& FocusedDetails() const { return focused_details_; }
    // The chat surface's view of FocusedDetails(), built with it -- once per
    // pass, not per render (ftxui renders on every event, mouse moves too).
    const plugins::ChatFrame& FocusedChatFrame() const { return focused_frame_; }

    // True once a scrollback refresh held back by the failure backoff is due
    // to retry. Nothing else wakes an idle frame for it (a failed fetch moves
    // no UiVersion once it has resolved), so the render pump polls this --
    // it is the one method safe to call from the pump thread.
    bool RefreshRetryDue(std::chrono::steady_clock::time_point now =
                             std::chrono::steady_clock::now()) const;

private:
    SessionState::Snapshot Maintain(const std::string& tab_id, SessionState::Snapshot snap);
    void HandleNotice(const SessionState::Snapshot& snap);
    // The permission/confirmation modals: rebuilt when the focused tab's
    // outstanding request changes, shown while unanswered. Only called when
    // `focused_` is non-empty (Run() resets both flags to false itself in
    // the no-tab case, mirroring Render()'s old two-branch structure).
    void MaintainModals(const SessionState::Snapshot& snap);

    Dependencies deps_;
    std::string focused_;
    SessionState::Snapshot focused_details_;
    plugins::ChatFrame focused_frame_;
    // Sentinel, not 0: forces the first call (before the loop starts) to
    // run regardless of whatever OpenTab's own bumps left UiVersion at.
    uint64_t last_ui_version_ = static_cast<uint64_t>(-1);
    // The focused tab's backoff-held refresh deadline (steady_clock ticks
    // since epoch), 0 when none is waiting. Atomic: read by the pump thread
    // through RefreshRetryDue.
    std::atomic<int64_t> refresh_retry_at_{0};
};

}  // namespace minimal::client
