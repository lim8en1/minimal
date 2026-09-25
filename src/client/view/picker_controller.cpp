#include "minimal/client/view/picker_controller.hpp"

#include <filesystem>
#include <iterator>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/client/cli_args.hpp"
#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/model_picker.hpp"
#include "minimal/client/view/session_picker.hpp"
#include "minimal/client/view/subagent_picker.hpp"

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
namespace {
constexpr int32_t kPickerPageSize = 100;
}  // namespace

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
    // Captured by value below: identifies THIS fetch so a reset that starts
    // a second, concurrent RunOnRpcQueue job (reset is exempt from the latch
    // above) can supersede it -- see picker_fetch_generation_'s doc comment.
    const uint64_t generation = ++picker_fetch_generation_;
    SessionClient::ListSessionsOptions opts;
    opts.project_root = CurrentProjectRootOrEmpty();
    opts.include_all_dirs = picker_data_.include_all_dirs;
    opts.limit = kPickerPageSize + 1;
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
            QueueApply([this, generation] {
                // A newer fetch (e.g. a reset that fired while this one was
                // outstanding) already owns the latch/model -- releasing it
                // here would be releasing state that isn't this fetch's to
                // release.
                if (generation != picker_fetch_generation_) return;
                picker_fetch_in_flight_ = false;
            });
            return;
        }
        const auto& page = result.response.sessions();
        const int64_t fetched = page.size();
        const bool has_more = fetched > kPickerPageSize;
        const int64_t take = has_more ? kPickerPageSize : fetched;
        std::vector<v1::SessionSummary> rows(page.begin(), page.begin() + take);
        QueueApply([this, rows = std::move(rows), has_more, generation]() mutable {
            // Superseded by a later fetch (typically a reset that fired
            // while this load-more was outstanding): applying these rows
            // now would append a stale, differently-paginated page onto a
            // list a newer fetch has already reset and is repopulating.
            if (generation != picker_fetch_generation_) return;
            picker_fetch_in_flight_ = false;
            picker_data_.has_more = has_more;
            picker_data_.sessions.insert(picker_data_.sessions.end(),
                                         std::make_move_iterator(rows.begin()),
                                         std::make_move_iterator(rows.end()));
            RebuildPickerComponent();
        });
    });
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
                  // No immediate RebuildPickerComponent here: unlike the
                  // ctrl+a toggle above, nothing observable changed yet (the
                  // component's row snapshot still matches picker_data_), so
                  // a rebuild would only throw away the user's filter text
                  // and cursor position -- then the fetch's arrival rebuild
                  // would do it a second time. The data-driven rebuild in
                  // the apply closure is the only one.
                  FetchPickerPage(/*reset=*/false);
              }});
}

void PickerController::OpenSessionPicker() {
    // T8: always a fresh first page in the project's own directory -- the
    // "start in the last-used all-dirs state" refinement is not worth the
    // surprise of a picker that sometimes silently shows other projects'
    // sessions.
    deps_.show_picker = true;
    picker_data_.include_all_dirs = false;
    FetchPickerPage(/*reset=*/true);
    RebuildPickerComponent();
}

// Mirrors RebuildPickerComponent, minus everything that doesn't apply to a
// small, unpaginated, unsorted list.
void PickerController::RebuildAgentPickerComponent() {
    *deps_.agent_picker_slot = MakeAgentPresetPicker(
        agent_picker_presets_, deps_.theme,
        {.on_select =
              [this](const std::string& name) {
                  // The DESIRED-STATE path, identical on every tab: the
                  // selection sets the tab's desired preset -- it rides
                  // CreateSessionRequest on a sessionless tab (the first
                  // message creates the session on it) and converges at the
                  // next turn start on a sessioned one. No command is sent,
                  // and no session is minted just to switch presets on it.
                  // focused_or_new_tab, not FocusedTabId: on the empty
                  // startup app (zero tabs -- how a bare /agent gets here)
                  // there is nothing focused, and the choice must land on
                  // the tab the user's first message will create.
                  const std::string tab = deps_.focused_or_new_tab();
                  if (!tab.empty()) {
                      deps_.app.StageAgentPreset(tab, name);
                      deps_.app.TabNotice(tab, "agent '" + name +
                                                   "' will be used on your next message");
                  }
                  deps_.show_agent_picker = false;
              },
          .on_close = [this] { deps_.show_agent_picker = false; }});
}

void PickerController::OpenAgentPicker() {
    // One fresh fetch each time, like OpenSessionPicker: the preset list is
    // small, so there is no pagination state to preserve across opens. The
    // fetch itself runs on AppState's RPC worker (see the file comment); the
    // picker opens showing whatever agent_picker_presets_ already holds
    // (empty on a fresh open) and repopulates the moment the fetch resolves.
    deps_.show_agent_picker = true;
    agent_picker_presets_.clear();
    RebuildAgentPickerComponent();
    deps_.app.RunOnRpcQueue([this] {
        // The client's cwd is the same project root session creation sends
        // (app.cpp), so the picker's project scope matches the sessions this
        // client creates here. A resumed session whose stored root differs
        // still resolves correctly server-side: /agent re-resolves against the
        // SESSION's root, and the resolved preset is what runs.
        const auto result =
            deps_.client.ListAgentPresets(std::filesystem::current_path().string());
        if (!result.status.ok()) {
            minimal::common::DefaultLogger().warn("agent picker fetch failed: {}",
                                                  result.status.error_message());
            return;
        }
        std::vector<v1::AgentPresetSummary> presets(result.response.presets().begin(),
                                                     result.response.presets().end());
        QueueApply([this, presets = std::move(presets)]() mutable {
            agent_picker_presets_ = std::move(presets);
            RebuildAgentPickerComponent();
        });
    });
}

// The model picker: bare /model opens with what the server already knows;
// ctrl:r from inside re-opens with discovery refreshed. `refresh_discovery`
// waits on every discover-entry's own catalog fetch (parallel server-side,
// bounded by the slowest provider's own timeout -- up to 15 s,
// session_client.cpp's own ListModels deadline), which is exactly why this
// runs on AppState's RPC worker rather than the loop thread: a 15 s stall
// used to freeze the whole TUI -- no redraw, no keystroke, no streaming
// progress -- for the duration.
void PickerController::OpenModelPicker(bool refresh) {
    model_picker_subagent_ = false;
    OpenModelPickerFetch(refresh);
}

void PickerController::OpenSubagentModelPicker() {
    model_picker_subagent_ = true;
    OpenModelPickerFetch(/*refresh=*/false);
}

void PickerController::OpenModelPickerFetch(bool refresh) {
    deps_.show_model_picker = true;
    model_picker_models_.clear();
    RebuildModelPickerComponent();
    SessionClient::ListModelsOptions opts;
    opts.refresh_discovery = refresh;
    deps_.app.RunOnRpcQueue([this, opts] {
        const auto result = deps_.client.ListModels(opts);
        if (!result.status.ok()) {
            minimal::common::DefaultLogger().warn("model picker fetch failed: {}",
                                                  result.status.error_message());
            return;
        }
        std::vector<v1::ModelSummary> models(result.response.models().begin(),
                                             result.response.models().end());
        QueueApply([this, models = std::move(models)]() mutable {
            model_picker_models_ = std::move(models);
            RebuildModelPickerComponent();
        });
    });
}

void PickerController::RebuildModelPickerComponent() {
    ModelPickerCallbacks callbacks;
    if (model_picker_subagent_) {
        callbacks.leading_row_label = "same as parent";
        callbacks.leading_row_id = kSameAsParentRowId;
    }
    callbacks.on_select =
        [this](const std::string& ref) {
            if (model_picker_subagent_) {
                // The DIRECT-COMMAND path: the subagent-model choice is a
                // session property, not desired state -- there is nothing to
                // stage (a sessionless tab has no session to set it on; the
                // picker only opens from a session's notice or a sessioned
                // tab's bare /subagent-model). The command's own result
                // surfaces as the tab notice.
                deps_.show_model_picker = false;
                if (const std::string tab = deps_.focused_or_new_tab(); !tab.empty()) {
                    deps_.app.SubmitCommand(tab,
                                            ref == kSameAsParentRowId
                                                ? std::string("/subagent-model inherit")
                                                : "/subagent-model " + ref);
                }
                return;
            }
            // The DESIRED-STATE path, identical on every tab (see the
            // agent picker above): the selection sets the tab's
            // desired model -- it rides CreateSessionRequest on a
            // sessionless tab and converges at the next turn start on
            // a sessioned one. No command, no direct switch.
            // focused_or_new_tab for the same empty-startup-app
            // reason the agent picker stages through it.
            const std::string tab = deps_.focused_or_new_tab();
            if (!tab.empty()) {
                deps_.app.StageModel(tab, ref);
                deps_.app.TabNotice(tab, "model '" + ref +
                                              "' will be used on your next message");
            }
            deps_.show_model_picker = false;
        };
    callbacks.on_refresh_discovery = [this] { OpenModelPicker(/*refresh=*/true); };
    callbacks.on_close = [this] { deps_.show_model_picker = false; };
    *deps_.model_picker_slot = MakeModelPicker(model_picker_models_, deps_.theme,
                                               std::move(callbacks));
}

// The /subagents picker (see the header). One bounded fetch per open, like
// the agent picker; the parent filter runs in the apply closure so a
// session's rows are the only ones this picker ever holds.
void PickerController::OpenSubagentsPicker(const std::string& parent_session_id) {
    deps_.show_subagent_picker = true;
    subagent_include_ended_ = false;
    subagent_parent_ = parent_session_id;
    subagent_children_.clear();
    RebuildSubagentPickerComponent();
    deps_.app.RunOnRpcQueue([this, parent_session_id] {
        // include_all_dirs: the GROUP's own project root governs where its
        // children live, not this client's cwd -- the same rule
        // AttachSessionGroup's child scan uses.
        SessionClient::ListSessionsOptions opts;
        opts.include_all_dirs = true;
        opts.include_subagents = true;
        opts.limit = 200;   // AttachSessionGroup's bound; one page, no load-more
        const auto result = deps_.client.ListSessions(opts);
        if (!result.status.ok()) {
            minimal::common::DefaultLogger().warn("subagent picker fetch failed: {}",
                                                  result.status.error_message());
            return;
        }
        std::vector<v1::SessionSummary> children;
        for (const auto& row : result.response.sessions()) {
            if (row.parent_session_id() == parent_session_id) children.push_back(row);
        }
        QueueApply([this, children = std::move(children)]() mutable {
            subagent_children_ = std::move(children);
            RebuildSubagentPickerComponent();
        });
    });
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
                                }});
}

}  // namespace minimal::client
