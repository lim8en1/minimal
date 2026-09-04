#include "minimal/client/view/picker_controller.hpp"

#include <filesystem>

#include "minimal/client/view/agent_picker.hpp"
#include "minimal/client/view/model_picker.hpp"
#include "minimal/client/view/session_picker.hpp"

namespace minimal::client {

PickerController::PickerController(Dependencies deps) : deps_(std::move(deps)) {}

// One page of the picker's list, kept synchronous like DeleteSession:
// unlike an async target (a per-frame Renderer call, a per-keystroke
// debounce), this runs once per explicit user action (open, toggle
// all-dirs, load more) -- the same bounded-pause trade the rest of RunTui's
// one-off calls make, and mutating picker_data_ / the picker slot from a
// worker thread would race the loop thread that owns both.
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
std::string PickerController::OpenPickedSession(const std::string& session_id) {
    for (const auto& session : picker_data_.sessions) {
        if (session.session_id() != session_id) continue;
        if (session.is_live()) {
            if (std::string attached = deps_.app.AttachTab(session_id); !attached.empty()) {
                return attached;
            }
            // is_live was stale -- the session ended between the list fetch
            // and this click (or this client already had the tab, which both
            // paths handle). Resume is the remaining option, and its failure
            // is visible (an Error-state tab), never silent.
        }
        return deps_.app.ResumeTab(session_id);
    }
    // Not in the fetched pages (a stale picker): resume is the only
    // meaningful try -- there is nothing to attach to that the list knows of.
    return deps_.app.ResumeTab(session_id);
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
    const auto result = deps_.client.ListSessions(opts);
    if (!result.status.ok()) return;
    const auto& page = result.response.sessions();
    const int64_t fetched = page.size();
    picker_data_.has_more = fetched > kPickerPageSize;
    const int64_t take = picker_data_.has_more ? kPickerPageSize : fetched;
    picker_data_.sessions.insert(picker_data_.sessions.end(), page.begin(), page.begin() + take);
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
                 // A PAST session opens into its own root group; it is not a
                 // child of whatever is focused. It never returns "".
                 deps_.app.SetFocusedTab(this->OpenPickedSession(session_id));
                 deps_.show_picker = false;
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
    // small, so there is no pagination state to preserve across opens.
    deps_.show_agent_picker = true;
    agent_picker_presets_.clear();
    const auto result = deps_.client.ListAgentPresets();
    if (result.status.ok()) {
        const auto& presets = result.response.presets();
        agent_picker_presets_.assign(presets.begin(), presets.end());
    }
    RebuildAgentPickerComponent();
}

// The model picker: bare /model opens with what the server already knows;
// ctrl:r from inside re-opens with discovery refreshed (the fetch can take
// seconds -- every provider's catalog, parallel server-side -- which is why
// it is an explicit action and not the open default).
void PickerController::OpenModelPicker(bool refresh) {
    deps_.show_model_picker = true;
    model_picker_models_.clear();
    SessionClient::ListModelsOptions opts;
    opts.refresh_discovery = refresh;
    const auto result = deps_.client.ListModels(opts);
    if (result.status.ok()) {
        const auto& models = result.response.models();
        model_picker_models_.assign(models.begin(), models.end());
    }
    RebuildModelPickerComponent();
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
