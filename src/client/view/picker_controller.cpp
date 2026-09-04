#include "minimal/client/view/picker_controller.hpp"

#include <filesystem>
#include <iterator>
#include <utility>

#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/model_picker.hpp"
#include "minimal/client/view/session_picker.hpp"

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

void PickerController::FetchPickerPage(bool reset) {
    if (reset) {
        picker_data_.sessions.clear();
        picker_data_.has_more = false;
    }
    SessionClient::ListSessionsOptions opts;
    opts.project_root = std::filesystem::current_path().string();
    opts.include_all_dirs = picker_data_.include_all_dirs;
    opts.limit = kPickerPageSize + 1;
    if (!reset && !picker_data_.sessions.empty()) {
        const auto& last = picker_data_.sessions.back();
        opts.before_updated_at = last.updated_at_unix_ms();
        opts.before_id = last.session_id();
    }
    deps_.app.RunOnRpcQueue([this, opts] {
        const auto result = deps_.client.ListSessions(opts);
        if (!result.status.ok()) return;   // silent: matches the old no-op-on-failure
        const auto& page = result.response.sessions();
        const int64_t fetched = page.size();
        const bool has_more = fetched > kPickerPageSize;
        const int64_t take = has_more ? kPickerPageSize : fetched;
        std::vector<v1::SessionSummary> rows(page.begin(), page.begin() + take);
        QueueApply([this, rows = std::move(rows), has_more]() mutable {
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
                 // own confirmation layer, styled like the quit one.
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
                 FetchPickerPage(/*reset=*/false);
                 RebuildPickerComponent();
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
                  // Sessioned tab: re-sends "/agent <name>" over the exact
                  // same path a typed command would take
                  // (AppState::SubmitCommand) -- AgentCommand::Execute's
                  // by-name branch, precedence rules and all, runs
                  // unchanged. No new mutation code. Sessionless tab (no
                  // session yet): the choice STAGES onto the tab's pending
                  // create-options -- it rides CreateSessionRequest when
                  // the first message creates the session -- instead of
                  // minting a session just to switch presets on it.
                  const std::string tab = deps_.app.FocusedTabId();
                  if (!tab.empty() && deps_.app.FocusedSessionId().empty()) {
                      deps_.app.StageAgentPreset(tab, name);
                      deps_.app.TabNotice(tab, "agent '" + name +
                                                   "' will be used when the session starts");
                  } else {
                      deps_.app.SubmitCommand(tab, "/agent " + name);
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
        if (!result.status.ok()) return;
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
    deps_.show_model_picker = true;
    model_picker_models_.clear();
    RebuildModelPickerComponent();
    SessionClient::ListModelsOptions opts;
    opts.refresh_discovery = refresh;
    deps_.app.RunOnRpcQueue([this, opts] {
        const auto result = deps_.client.ListModels(opts);
        if (!result.status.ok()) return;
        std::vector<v1::ModelSummary> models(result.response.models().begin(),
                                             result.response.models().end());
        QueueApply([this, models = std::move(models)]() mutable {
            model_picker_models_ = std::move(models);
            RebuildModelPickerComponent();
        });
    });
}

void PickerController::RebuildModelPickerComponent() {
    *deps_.model_picker_slot = MakeModelPicker(
        model_picker_models_, deps_.theme,
        {.on_select =
              [this](const std::string& ref) {
                  // Sessioned tab: re-sends "/model <ref>" over the exact
                  // same path a typed command would take
                  // (AppState::SubmitCommand) -- ModelCommand::Execute's
                  // by-ref branch, precedence rules and all, runs
                  // unchanged. No new mutation code. Sessionless tab: the
                  // choice stages onto the tab's pending create-options
                  // (validated by the server at CreateSession), exactly
                  // like the agent picker above.
                  const std::string tab = deps_.app.FocusedTabId();
                  if (!tab.empty() && deps_.app.FocusedSessionId().empty()) {
                      deps_.app.StageModel(tab, ref);
                      deps_.app.TabNotice(tab, "model '" + ref +
                                                   "' will be used when the session starts");
                  } else {
                      deps_.app.SubmitCommand(tab, "/model " + ref);
                  }
                  deps_.show_model_picker = false;
              },
         .on_refresh_discovery = [this] { OpenModelPicker(/*refresh=*/true); },
         .on_close = [this] { deps_.show_model_picker = false; }});
}

}  // namespace minimal::client
