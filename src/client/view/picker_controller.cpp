#include "minimal/client/view/picker_controller.hpp"

#include <iterator>
#include <optional>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/client/command_line.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/subagent_picker.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client {

PickerController::PickerController(Dependencies deps) : deps_(std::move(deps)) {}

// One page of the picker's list. The RPC runs on AppState's RPC worker (see
// the file comment); this function itself still runs on the loop thread --
// it only builds the request (from picker_data_, loop-thread-owned) and, for
// a reset, clears picker_data_ synchronously so the caller's immediate
// RebuildPickerComponent() shows the cleared state right away, before the
// fetch resolves.
//
// Requests one row beyond the page size to detect a further page: the wire
// has no dedicated has_more field on ListSessionsResponse.

// The picker's open action, decided per ROW: a LIVE session (is_live -- a
// Session object still exists server-side, attached or detached) ATTACHES to
// the running instance, the way the row's label has always claimed; a stored
// one RESUMES from the transcript. Routing a live session through ResumeTab
// instead made the server refuse the resume with "already open" -- the exact
// "resume failed" a user saw after quitting with ctrl+c (which detaches, and
// leaves every session live) and reopening the client.
//
// Runs on AppState's RPC worker (see the file comment) -- AttachTab/ResumeTab
// carry 5 s deadlines, which used to block the loop thread on every open.
// `is_live` is resolved by the CALLER, on the loop thread, before this is
// queued -- `is_live` false (not found in the fetched pages: a stale picker,
// or the picker was never asked) falls straight through to resume, the only
// meaningful try when there is nothing to attach to that the list knows of.
std::string PickerController::OpenPickedSession(const std::string& session_id, bool is_live) {
    if (is_live) {
        if (std::string attached = deps_.app.AttachTab(session_id); !attached.empty()) {
            return attached;
        }
        // is_live was stale -- the session ended between the list fetch and
        // this click (or this client already had the tab, which both paths
        // handle). Resume is the remaining option, and its failure is
        // visible (an Error-state tab), never silent.
    }
    return deps_.app.ResumeTab(session_id);
}

template <typename T>
void PickerController::RunFetch(FetchState& state,
                                std::function<std::optional<T>(std::string& error)> fetch,
                                std::function<void(T&&)> apply, std::function<void()> rebuild,
                                const char* what) {
    deps_.app.RunOnRpcQueue(
        [this, &state, fetch = std::move(fetch), apply = std::move(apply),
         rebuild = std::move(rebuild), what] {
            std::string error;
            std::optional<T> data = fetch(error);
            if (!data) {
                minimal::common::DefaultLogger().warn("{} fetch failed: {}", what, error);
                if (error.empty()) error = "the server did not answer";
            }
            // `state` is loop-thread data: touched only inside the apply.
            QueueApply([&state, data = std::move(data), error, apply, rebuild]() mutable {
                state.loading = false;
                if (data) {
                    state.error.clear();
                    apply(std::move(*data));
                } else {
                    state.error = error;
                }
                rebuild();
            });
        },
        /*must_run=*/true, AppState::RpcLane::kBulk);
}

void PickerController::QueueApply(std::function<void()> apply) {
    {
        std::lock_guard<std::mutex> lock(async_mtx_);
        pending_apply_.push_back(std::move(apply));
    }
    // Same counter every ordinary AppState mutation bumps -- wakes the 20 Hz
    // pump so a frame lands with Poll() able to drain this.
    deps_.app.NotifyExternalUpdate();
}

void PickerController::Poll() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(async_mtx_);
        if (pending_apply_.empty()) return;
        ready.swap(pending_apply_);
    }
    for (auto& apply : ready) apply();   // loop thread only, from here on
}

// The delete confirmation's resolution path: a confirmed delete must not
// leave the removed row on screen, so the still-open picker re-fetches from
// scratch. Called from RunTui's delete_modal callback on the loop thread --
// FetchPickerPage(reset) clears picker_data_ synchronously (like the ctrl+a
// toggle) and the fetch's apply closure rebuilds the component when the page
// lands.
void PickerController::RefreshSessionPicker() {
    FetchPickerPage(/*reset=*/true);
    RebuildPickerComponent();
}

void PickerController::FetchPickerPage(bool reset) {
    if (reset) {
        picker_data_.sessions.clear();
        picker_data_.has_more = false;
        session_fetch_.Begin(/*keep_input=*/true);
    } else if (picker_fetch_in_flight_) {
        // A second load-more for the SAME cursor (key-repeat queues it faster
        // than the round trip) must not be queued: both arrivals would append
        // the identical page and duplicate every row. The latch is released
        // when the outstanding fetch's apply (or failure) lands; a RESET
        // fetch is exempt -- it clears the model synchronously and carries a
        // different cursor, so blocking it would strand the picker. Exempting
        // it is safe: picker_fetch_generation_ (bumped below) supersedes
        // whatever this latch was guarding the moment the reset proceeds.
        return;
    }
    picker_fetch_in_flight_ = true;
    session_fetch_.loading = true;
    // Captured by value below: identifies THIS fetch so a reset that starts
    // a second, concurrent RunOnRpcQueue job (reset is exempt from the latch
    // above) can supersede it -- see picker_fetch_generation_'s doc comment.
    const uint64_t generation = ++picker_fetch_generation_;
    SessionClient::ListSessionsOptions opts;
    opts.project_root = CurrentProjectRootOrEmpty();
    opts.include_all_dirs = picker_data_.include_all_dirs;
    opts.limit = kSessionListPageSize + 1;
    if (!reset && !picker_data_.sessions.empty()) {
        const auto& last = picker_data_.sessions.back();
        opts.before_updated_at = last.updated_at_unix_ms();
        opts.before_id = last.session_id();
    }
    deps_.app.RunOnRpcQueue([this, opts, generation] {
        const auto result = deps_.client.ListSessions(opts);
        if (!result.status.ok()) {
            // Still a no-op for the LIST (the latch must release, or every
            // later load-more is dropped forever -- loop-thread state, so
            // the release goes through QueueApply), but no longer silent:
            // an operator debugging a dead server gets one line per fetch.
            minimal::common::DefaultLogger().warn("session picker fetch failed: {}",
                                                  result.status.error_message());
            QueueApply([this, generation, error = result.status.error_message()] {
                // A newer fetch (e.g. a reset that fired while this one was
                // outstanding) already owns the latch/model -- releasing it
                // here would be releasing state that isn't this fetch's to
                // release.
                if (generation != picker_fetch_generation_) return;
                picker_fetch_in_flight_ = false;
                session_fetch_.loading = false;
                session_fetch_.error = error.empty() ? "the server did not answer" : error;
                RebuildPickerComponent();
            });
            return;
        }
        const auto& page = result.response.sessions();
        const int64_t fetched = page.size();
        const bool has_more = fetched > kSessionListPageSize;
        const int64_t take = has_more ? kSessionListPageSize : fetched;
        std::vector<v1::SessionSummary> rows(page.begin(), page.begin() + take);
        QueueApply([this, rows = std::move(rows), has_more, generation]() mutable {
            // Superseded by a later fetch (typically a reset that fired
            // while this load-more was outstanding): applying these rows
            // now would append a stale, differently-paginated page onto a
            // list a newer fetch has already reset and is repopulating.
            if (generation != picker_fetch_generation_) return;
            picker_fetch_in_flight_ = false;
            session_fetch_.loading = false;
            session_fetch_.error.clear();
            picker_data_.has_more = has_more;
            picker_data_.sessions.insert(picker_data_.sessions.end(),
                                         std::make_move_iterator(rows.begin()),
                                         std::make_move_iterator(rows.end()));
            RebuildPickerComponent();
        });
    }, /*must_run=*/true, AppState::RpcLane::kBulk);
}

// Rebuilds the picker component from picker_data_'s CURRENT contents --
// every mutating action (resume/delete/new leave the modal; toggling
// all-dirs or loading more re-fetches and rebuilds in place, same as the
// rest of RunTui's "modal over data it does not own" pattern).
void PickerController::RebuildPickerComponent() {
    *deps_.picker_slot = MakeSessionPicker(
        picker_data_.sessions, picker_data_.include_all_dirs, picker_data_.has_more, deps_.theme,
        {.on_resume =
             [this](const std::string& session_id) {
                 // is_live is resolved HERE, on the loop thread, from
                 // picker_data_ (loop-thread-owned) -- OpenPickedSession runs
                 // on the RPC worker (AttachTab/ResumeTab carry a 5 s
                 // deadline) and must not read picker_data_ itself, which
                 // Poll() can be concurrently rewriting.
                 bool is_live = false;
                 for (const auto& session : picker_data_.sessions) {
                     if (session.session_id() == session_id) {
                         is_live = session.is_live();
                         break;
                     }
                 }
                 // Close immediately -- don't make the user wait on the
                 // network to see the modal go away. A PAST session opens
                 // into its own root group; it is not a child of whatever is
                 // focused. OpenPickedSession never returns "".
                 deps_.show_picker = false;
                 deps_.app.RunOnRpcQueue([this, session_id, is_live] {
                     deps_.app.SetFocusedTab(this->OpenPickedSession(session_id, is_live));
                 });
             },
         .on_delete =
              [this](const std::string& session_id) {
                  // Deletion is destructive and permanent, so it asks -- its
                  // own confirmation layer, styled like the quit one. The
                  // confirm sits BELOW this picker in RunTui's modal stack
                  // (app.cpp), so the picker must hide itself for the
                  // confirm's duration; RunTui's delete_modal callback
                  // restores show_picker when the confirm resolves, so the
                  // user lands back here (a confirmed delete also triggers
                  // RefreshSessionPicker to drop the removed row).
                  deps_.staged_delete_id = session_id;
                  deps_.show_delete_confirm = true;
                  deps_.show_picker = false;
              },
         .on_new = deps_.open_new_session,
         .on_close = [this] { deps_.show_picker = false; },
         .on_toggle_all_dirs =
             [this] {
                 picker_data_.include_all_dirs = !picker_data_.include_all_dirs;
                 FetchPickerPage(/*reset=*/true);
                 RebuildPickerComponent();
             },
          .on_load_more =
              [this] {
                  // Rebuilt at once for the "loading more…" line; the menu's
                  // filter and cursor ride session_fetch_.memory across it.
                  FetchPickerPage(/*reset=*/false);
                  RebuildPickerComponent();
              }},
        session_fetch_.Status());
}

void PickerController::OpenSessionPicker() {
    // T8: always a fresh first page in the project's own directory -- the
    // "start in the last-used all-dirs state" refinement is not worth the
    // surprise of a picker that sometimes silently shows other projects'
    // sessions.
    deps_.show_picker = true;
    picker_data_.include_all_dirs = false;
    session_fetch_.Begin();
    FetchPickerPage(/*reset=*/true);
    RebuildPickerComponent();
}

// The desired-state choices -- the view registered as "agents" / "models"
// (a plugin's) calls these through HostActions. The DESIRED-STATE path,
// identical on every tab: the choice sets the tab's desired preset or model,
// which rides CreateSessionRequest on a sessionless tab (the first message
// creates the session on it) and converges at the next turn start on a
// sessioned one. No command is sent, and no session is minted just to
// switch. focused_or_new_tab, not FocusedTabId: on the empty startup app
// (zero tabs -- how a bare /agent gets here) there is nothing focused, and
// the choice must land on the tab the user's first message will create.
void PickerController::ChooseAgent(const std::string& name) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.StageAgentPreset(tab, name);
        deps_.app.TabNotice(tab, "agent '" + name + "' will be used on your next message");
    }
    ContinuePendingSelection(/*chosen=*/true);
}

void PickerController::ChooseModel(const std::string& ref) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.StageModel(tab, ref);
        deps_.app.TabNotice(tab, "model '" + ref + "' will be used on your next message");
    }
    ContinuePendingSelection(/*chosen=*/true);
}

// Back to the default model (settings.json default_model, or the only
// model): clears the tab's own choice. `default_ref` is what the server
// named the default, for the status bar until the session confirms it.
void PickerController::ChooseDefaultModel(const std::string& default_ref) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.StageModel(tab, default_ref, /*is_default=*/true);
        deps_.app.TabNotice(tab, "the default model will be used on your next message");
    }
    ContinuePendingSelection(/*chosen=*/true);
}

// NOT desired state: the subagent model is a session's own setting, so it
// is the /subagent-model command (whose result surfaces as the tab notice).
void PickerController::SetSubagentModel(const std::string& ref) {
    if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
        deps_.app.SubmitCommand(tab, "/subagent-model " + ref);
    }
}

void PickerController::OpenAgentPicker() { OpenRole("agents"); }

void PickerController::OpenModelPicker() { OpenRole("models"); }

void PickerController::OpenRole(const std::string& role) {
    if (!deps_.open_role_view(role)) {
        // Nothing to choose with (open_role_view said so in a notice): a
        // first message waiting on a choice cannot proceed.
        if (pending_selection_) ContinuePendingSelection(/*chosen=*/false);
        return;
    }
    if (pending_selection_) pending_selection_->awaiting = role;
}

void PickerController::WatchPendingView(const std::string& shown_modal) {
    // The awaited view went away -- Escape (the host's), or another modal
    // replaced it -- with no choice having moved the selection on: the
    // message is not sent.
    if (!pending_selection_ || pending_selection_->awaiting.empty()) return;
    if (shown_modal == pending_selection_->awaiting) return;
    ContinuePendingSelection(/*chosen=*/false);
}

// The /subagents picker (see the header). One full listing per open, filtered
// by parent as it pages, so a session's rows are the only ones this picker
// ever holds.
void PickerController::OpenSubagentsPicker(const std::string& parent_session_id) {
    deps_.show_subagent_picker = true;
    subagent_include_ended_ = false;
    subagent_parent_ = parent_session_id;
    subagent_children_.clear();
    subagent_fetch_.Begin();
    RebuildSubagentPickerComponent();
    RunFetch<std::vector<v1::SessionSummary>>(
        subagent_fetch_,
        [this, parent_session_id](std::string& error)
            -> std::optional<std::vector<v1::SessionSummary>> {
            // include_all_dirs: the GROUP's own project root governs where
            // its children live, not this client's cwd (AttachSessionGroup's
            // rule). Every page, filtered as it arrives: one 200-row page
            // silently dropped the children of any session older than the
            // 200 most recently updated.
            SessionClient::ListSessionsOptions opts;
            opts.include_all_dirs = true;
            opts.include_subagents = true;
            auto result = deps_.client.ListAllSessions(opts, [&](const v1::SessionSummary& row) {
                return row.parent_session_id() == parent_session_id;
            });
            if (!result.status.ok()) {
                error = result.status.error_message();
                return std::nullopt;
            }
            return std::move(result.sessions);
        },
        [this](std::vector<v1::SessionSummary>&& children) {
            subagent_children_ = std::move(children);
        },
        [this] { RebuildSubagentPickerComponent(); }, "subagent picker");
}

void PickerController::RebuildSubagentPickerComponent() {
    *deps_.subagent_picker_slot =
        MakeSubagentPicker(subagent_children_, subagent_include_ended_, deps_.theme,
                           {.on_select =
                                [this](const std::string& session_id, bool is_live) {
                                    deps_.show_subagent_picker = false;
                                    const std::string parent = subagent_parent_;
                                    deps_.app.RunOnRpcQueue([this, session_id, is_live, parent] {
                                        // Already open -> focus it. Everything
                                        // downstream (AttachTab's duplicate rule,
                                        // OpenTranscriptTab's) agrees: one tab per
                                        // session, the existing one is the answer.
                                        for (const auto& tab : deps_.app.GetAllTabs()) {
                                            if (tab.session_id == session_id) {
                                                deps_.app.SetFocusedTab(tab.tab_id);
                                                return;
                                            }
                                        }
                                        if (is_live) {
                                            // Group under the parent's root -- the
                                            // same ordinal lookup OnSubagentSpawned
                                            // does, so the attached child takes its
                                            // "↳" place in the group.
                                            int64_t root_ordinal = -1;
                                            for (const auto& tab : deps_.app.GetAllTabs()) {
                                                if (tab.session_id == parent) {
                                                    root_ordinal = tab.root_ordinal;
                                                    break;
                                                }
                                            }
                                            if (std::string attached =
                                                    deps_.app.AttachTab(session_id, root_ordinal);
                                                !attached.empty()) {
                                                deps_.app.SetFocusedTab(attached);
                                                return;
                                            }
                                            // is_live went stale between the fetch
                                            // and this click (the child's turn just
                                            // ended): the stored transcript is
                                            // what remains to show -- fall through.
                                        }
                                        if (std::string opened =
                                                deps_.app.OpenTranscriptTab(session_id, parent);
                                            !opened.empty()) {
                                            deps_.app.SetFocusedTab(opened);
                                        }
                                    });
                                },
                            .on_close = [this] { deps_.show_subagent_picker = false; },
                            .on_toggle_ended =
                                [this] {
                                    subagent_include_ended_ = !subagent_include_ended_;
                                    // Pure display toggle: the list was fetched
                                    // whole, only the row filter changes -- no
                                    // re-fetch, exactly like the session picker's
                                    // local Tab sort (and unlike its ctrl+a, which
                                    // changes what the SERVER would send).
                                    RebuildSubagentPickerComponent();
                                }},
                           subagent_fetch_.Status());
}

void PickerController::EnsureSelectionThen(const std::string& tab, std::function<void()> send,
                                           std::function<void()> cancel) {
    const auto selection = deps_.app.SelectionFor(tab);
    const bool has_agent = !selection.agent.empty();
    const bool has_model = !selection.model.empty() && !selection.model_default;
    if (!selection.exists || selection.sessioned || (has_agent && has_model)) {
        send();
        return;
    }
    // Whether the server has a default is its answer (settings, or the only
    // option), asked fresh: discovery may have filled the model list since.
    const std::string root = selection.project_root;
    deps_.app.RunOnRpcQueue(
        [this, tab, root, has_agent, has_model, send = std::move(send),
         cancel = std::move(cancel)]() mutable {
            std::optional<v1::ListAgentPresetsResponse> agents;
            std::optional<v1::ListModelsResponse> models;
            if (!has_agent) {
                auto result = deps_.client.ListAgentPresets(root);
                if (result.status.ok()) agents = std::move(result.response);
            }
            if (!has_model) {
                auto result = deps_.client.ListModels({});
                if (result.status.ok()) models = std::move(result.response);
            }
            QueueApply([this, tab, agents = std::move(agents), models = std::move(models),
                        send = std::move(send), cancel = std::move(cancel)]() mutable {
                PendingSelection pending{tab, std::move(send), std::move(cancel)};
                // Ask only when there is a choice and no default. A failed
                // listing, or nothing to choose from, is left to the create's
                // own error, which names the fix.
                pending.need_agent =
                    agents && agents->default_agent_preset().empty() && agents->presets_size() > 0;
                pending.need_model =
                    models && models->default_model().empty() && models->models_size() > 0;
                if (!pending.need_agent && !pending.need_model) {
                    pending.send();
                    return;
                }
                if (pending_selection_ && pending_selection_->cancel) pending_selection_->cancel();
                pending_selection_ = std::move(pending);
                ContinuePendingSelection(/*chosen=*/true);
            });
        },
        /*must_run=*/true, AppState::RpcLane::kBulk);
}

void PickerController::ContinuePendingSelection(bool chosen) {
    if (!pending_selection_) return;
    pending_selection_->awaiting.clear();
    if (!chosen) {
        auto cancel = std::move(pending_selection_->cancel);
        pending_selection_.reset();
        if (cancel) cancel();
        return;
    }
    const auto& pending = *pending_selection_;
    const auto selection = deps_.app.SelectionFor(pending.tab);
    if (pending.need_agent && selection.agent.empty()) {
        deps_.app.TabNotice(pending.tab,
                            "choose an agent for this project (remembered for next time) -- "
                            "your message is sent once you have");
        OpenRole("agents");
        return;
    }
    if (pending.need_model && (selection.model.empty() || selection.model_default)) {
        deps_.app.TabNotice(pending.tab,
                            "choose a model for this project (remembered for next time) -- "
                            "your message is sent once you have");
        OpenRole("models");
        return;
    }
    auto send = std::move(pending_selection_->send);
    pending_selection_.reset();
    send();
}

}  // namespace minimal::client
