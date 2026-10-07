#include "model_selector.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/component_base.hpp>

#include "model_picker.hpp"

namespace minimal::client::ui_kit {
namespace {

// What the worker publishes and the component reads; shared, so a task
// still running when the component goes away writes into something alive.
struct SelectorState {
    std::mutex mtx;
    std::vector<v1::ModelSummary> models;
    std::string default_model;  // the answer's, "" when it names none
    bool loading = true;
    std::string error;
    std::uint64_t generation = 1;  // bumped by every change
};

class ModelSelectorComponent final : public ftxui::ComponentBase {
public:
    ModelSelectorComponent(ModelSelectorSpec spec, ModelSelectorCallbacks callbacks,
                           const Theme& theme)
        : spec_(std::move(spec)),
          callbacks_(std::move(callbacks)),
          theme_(theme) {
        Fetch(/*refresh=*/false);
    }

private:
    void Fetch(bool refresh) {
        {
            std::lock_guard<std::mutex> lock(state_->mtx);
            state_->loading = true;
            state_->error.clear();
            ++state_->generation;
        }
        if (!spec_.post) {
            std::lock_guard<std::mutex> lock(state_->mtx);
            state_->loading = false;
            state_->error = "no worker to fetch the model list on";
            ++state_->generation;
            return;
        }
        // Shared state and copies only: the task may outlive this component.
        spec_.post([state = state_, fetch = spec_.fetch, redraw = spec_.redraw, refresh] {
            std::string error;
            std::optional<v1::ListModelsResponse> answer;
            if (fetch) {
                answer = fetch(refresh, error);
            } else {
                error = "no model source";
            }
            {
                std::lock_guard<std::mutex> lock(state->mtx);
                if (answer) {
                    state->models.assign(answer->models().begin(), answer->models().end());
                    state->default_model = answer->default_model();
                    state->error.clear();
                } else {
                    state->error = error.empty() ? "the model list is unavailable" : error;
                }
                state->loading = false;
                ++state->generation;
            }
            if (redraw) redraw();
        });
    }

    // Rebuilds the picker if a newer answer landed -- at the start of a
    // render or an event only, never from ActiveChild() (called mid-frame:
    // a rebuild there would free the component being drawn).
    void Sync() {
        std::vector<v1::ModelSummary> models;
        std::string default_model;
        MenuStatus status;
        {
            std::lock_guard<std::mutex> lock(state_->mtx);
            if (shown_ && state_->generation == built_generation_) return;
            built_generation_ = state_->generation;
            models = state_->models;
            default_model = state_->default_model;
            status.loading = state_->loading;
            status.error = state_->error;
        }
        status.memory = memory_;
        std::string leading_label = spec_.leading_row_label;
        std::string leading_id = spec_.leading_row_id;
        if (spec_.leading_row_is_default) {
            leading_label = "default (" + default_model + ")";
            if (default_model.empty()) leading_id.clear();  // no default: no row
        }
        // A model's own name when asked for one: the listed model's, else the
        // ref's model part; the leading row and a typed value pass as they are.
        auto on_select = callbacks_.on_select;
        if (spec_.choose_model_name && on_select) {
            on_select = [on_select, models](const std::string& ref) {
                for (const auto& model : models) {
                    if (model.ref() != ref) continue;
                    const auto slash = ref.find('/');
                    on_select(!model.model_name().empty() ? model.model_name()
                              : slash != std::string::npos ? ref.substr(slash + 1)
                                                           : ref);
                    return;
                }
                on_select(ref);
            };
        }
        ModelPickerCallbacks picker{
            .on_select = std::move(on_select),
            .on_refresh_discovery = [this] { Fetch(/*refresh=*/true); },
            .on_close = callbacks_.on_close,
            .leading_row_label = std::move(leading_label),
            .leading_row_id = std::move(leading_id),
            .allow_custom = spec_.allow_custom,
            .custom_is_name = spec_.choose_model_name,
            .window_title = spec_.window_title,
            .select_hint = spec_.select_hint,
        };
        if (shown_) DetachAllChildren();
        shown_ = MakeModelPicker(models, theme_, std::move(picker), std::move(status));
        Add(shown_);
    }

    ftxui::Element OnRender() override {
        Sync();
        return shown_->Render();
    }
    bool OnEvent(ftxui::Event event) override {
        Sync();
        const ftxui::Component target = shown_;  // a callback may rebuild
        return target->OnEvent(std::move(event));
    }
    bool Focusable() const override { return true; }
    ftxui::Component ActiveChild() override { return shown_; }

    ModelSelectorSpec spec_;
    ModelSelectorCallbacks callbacks_;
    const Theme& theme_;
    std::shared_ptr<SelectorState> state_ = std::make_shared<SelectorState>();
    std::shared_ptr<MenuMemory> memory_ = std::make_shared<MenuMemory>();
    ftxui::Component shown_;
    std::uint64_t built_generation_ = 0;
};

}  // namespace

ftxui::Component MakeModelSelector(ModelSelectorSpec spec, ModelSelectorCallbacks callbacks,
                                   const Theme& theme) {
    return ftxui::Make<ModelSelectorComponent>(std::move(spec), std::move(callbacks), theme);
}

}  // namespace minimal::client::ui_kit
