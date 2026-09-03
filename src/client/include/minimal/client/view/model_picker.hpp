#pragma once

// The model picker modal: a 2-tier (provider -> model) filterable menu over
// a ListModels snapshot. Same "modal over data it does not own" shape as
// MakeSessionPicker: the row building is pure (model_picker_model.cpp), the
// component is rebuilt in place by PickerController whenever the underlying
// list changes (refresh discovery), and selection just reports the chosen
// ref -- the switch itself re-runs the /model command through the normal
// dispatch path, server-side rules included.

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct ModelPickerCallbacks {
    std::function<void(const std::string& ref)> on_select;
    // ctrl+r: re-fetch the model list with discovery refreshed. The caller
    // owns the fetch (it can take seconds; every provider's catalog is
    // fetched in parallel server-side) and rebuilds the component in place.
    std::function<void()> on_refresh_discovery;
    std::function<void()> on_close;   // Escape
};

ftxui::Component MakeModelPicker(const std::vector<v1::ModelSummary>& models,
                                 const Theme& theme, ModelPickerCallbacks callbacks);

}  // namespace minimal::client
