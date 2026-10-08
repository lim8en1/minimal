#include "selection_view.hpp"

#include <mutex>
#include <utility>

#include "minimal/client/sdk/picker_model.hpp"
#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/sdk/rebuilding.hpp"

namespace minimal::selection_view {

// The model chooser's non-model rows: not refs (no '/'), so no model can
// collide with them.
namespace {
constexpr const char* kDefaultRowId = "<default>";
constexpr const char* kInheritRowId = "<inherit>";
}  // namespace

std::vector<client::MenuRow> BuildAgentRows(const std::vector<v1::AgentPresetSummary>& presets,
                                            const std::string& filter) {
    const std::string needle = client::ToLower(filter);
    std::vector<client::MenuRow> rows;
    for (const auto& preset : presets) {
        if (!client::MatchesFilter(needle, {preset.name(), preset.description()})) continue;
        std::string label = preset.name();
        if (!preset.description().empty()) label += "  ·  " + preset.description();
        rows.push_back({std::move(label), preset.name(), /*header=*/false, preset.color()});
    }
    return rows;
}

struct AgentsState {
    std::mutex mtx;
    std::vector<v1::AgentPresetSummary> presets;
    bool loading = true;
    std::string error;
    // The filter and cursor, kept across the rebuild an arriving list causes.
    std::shared_ptr<client::MenuMemory> memory = std::make_shared<client::MenuMemory>();
};

AgentsView::AgentsView(Seams seams)
    : seams_(seams),
      state_(std::make_shared<AgentsState>()),
      worker_(std::make_unique<plugins::TaskWorker>("selection-view", [ui = seams.ui] {
          ui->RequestRedraw();
      })) {}

AgentsView::~AgentsView() = default;

const plugins::ViewSpec& AgentsView::spec() const {
    static const plugins::ViewSpec kSpec{kAgentsViewId, "switch agent preset",
                                         plugins::ViewPlacement::Modal, {}, kAgentsViewId};
    return kSpec;
}

void AgentsView::OnShow() const {
    {
        std::lock_guard<std::mutex> lock(state_->mtx);
        state_->loading = true;
        state_->error.clear();
        state_->presets.clear();
        state_->memory = std::make_shared<client::MenuMemory>();  // a fresh open, a fresh filter
    }
    stale_->store(true);
    worker_->Post([state = state_, stale = stale_, api = seams_.api] {
        std::string error;
        auto answer = api->ListAgentPresets(&error);
        std::lock_guard<std::mutex> lock(state->mtx);
        state->loading = false;
        if (answer) {
            state->presets.assign(answer->presets().begin(), answer->presets().end());
        } else {
            state->error = error.empty() ? "the agent list is unavailable" : error;
        }
        stale->store(true);  // rebuilt with the list at the next frame
    });
}

ftxui::Component AgentsView::component() {
    return ftxui::Make<client::RebuildingComponent>(stale_, [seams = seams_, state = state_,
                                                             theme = &theme_] {
        std::vector<v1::AgentPresetSummary> presets;
        client::FilterableMenuSpec spec{
            .window_title = "switch agent preset",
            .hint_line = " enter: switch  ·  esc: close",
            .empty_message = " (no agent presets match)",
        };
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            presets = state->presets;
            spec.status = {state->loading, state->error, state->memory};
        }
        const auto close = [ui = seams.ui] { ui->CloseView(kAgentsViewId); };
        return seams.widgets->FilterableMenuModal(
            [presets = std::move(presets)](const std::string& filter) {
                return BuildAgentRows(presets, filter);
            },
            spec,
            {.on_select =
                 [actions = seams.actions, close](const client::MenuRow& row) {
                     actions->ChooseAgent(row.id);
                     close();
                 },
             .on_close = close},
            *theme);
    });
}

ModelsView::ModelsView(Seams seams, bool for_subagents)
    : seams_(seams),
      for_subagents_(for_subagents),
      worker_(std::make_unique<plugins::TaskWorker>("selection-view models")) {}

ModelsView::~ModelsView() = default;

const plugins::ViewSpec& ModelsView::spec() const {
    static const plugins::ViewSpec kModels{kModelsViewId, "switch model",
                                           plugins::ViewPlacement::Modal, {}, kModelsViewId};
    static const plugins::ViewSpec kSubagents{kSubagentModelsViewId, "subagent model",
                                              plugins::ViewPlacement::Modal, {},
                                              kSubagentModelsViewId};
    return for_subagents_ ? kSubagents : kModels;
}

ftxui::Component ModelsView::component() {
    plugins::TaskWorker* worker = worker_.get();
    return ftxui::Make<client::RebuildingComponent>(stale_, [seams = seams_,
                                                             subagents = for_subagents_,
                                                             theme = &theme_, worker] {
        const char* view_id = subagents ? kSubagentModelsViewId : kModelsViewId;
        // What the server names its default, from the fetch, for the
        // default row's choice.
        auto default_ref = std::make_shared<std::string>();
        auto guard = std::make_shared<std::mutex>();
        client::ModelSelectorSpec spec;
        spec.window_title = subagents ? "subagent model" : "switch model";
        spec.select_hint = "switch";
        spec.fetch = [api = seams.api, default_ref, guard](bool refresh, std::string& error) {
            auto answer = api->ListModels(refresh, &error);
            if (answer) {
                std::lock_guard<std::mutex> lock(*guard);
                *default_ref = answer->default_model();
            }
            return answer;
        };
        spec.post = [worker](std::function<void()> task) { worker->Post(std::move(task)); };
        spec.redraw = [ui = seams.ui] { ui->RequestRedraw(); };
        spec.leading_row_label = "same as parent";  // the subagent form's
        spec.leading_row_id = subagents ? kInheritRowId : kDefaultRowId;
        spec.leading_row_is_default = !subagents;
        spec.allow_custom = true;
        const auto close = [ui = seams.ui, view_id] { ui->CloseView(view_id); };
        return seams.widgets->ModelSelector(
            std::move(spec),
            {.on_select =
                 [actions = seams.actions, subagents, default_ref, guard,
                  close](const std::string& ref) {
                     if (subagents) {
                         actions->SetSubagentModel(ref == kInheritRowId ? "inherit" : ref);
                     } else if (ref == kDefaultRowId) {
                         std::lock_guard<std::mutex> lock(*guard);
                         actions->ChooseDefaultModel(*default_ref);
                     } else {
                         actions->ChooseModel(ref);
                     }
                     close();
                 },
             .on_close = close},
            *theme);
    });
}

}  // namespace minimal::selection_view
