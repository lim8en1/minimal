#include "minimal/client/view/picker_controller.hpp"

#include <filesystem>

#include "minimal/client/view/agent_picker.hpp"
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
                 // A PAST session resumes into its own root group; it is
                 // not a child of whatever is focused. ResumeTab re-issues
                 // CreateSession with resume_session_id -- the server must
                 // revive the stored session before the stream names it
                 // (bare attach answered NOT_FOUND and the dead stream then
                 // swallowed every prompt) -- and registers a refused
                 // resume as a visible Error-state tab instead of
                 // dropping the pick on the floor. It never returns "".
                 deps_.app.SetFocusedTab(deps_.app.ResumeTab(session_id));
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
                 // Re-sends "/agent <name>" over the exact same path a
                 // typed command would take (AppState::SendCommand) --
                 // AgentCommand::Execute's by-name branch, precedence rules
                 // and all, runs unchanged. No new mutation code.
                 deps_.app.SendCommand(deps_.app.FocusedSessionId(), "/agent " + name);
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

}  // namespace minimal::client
