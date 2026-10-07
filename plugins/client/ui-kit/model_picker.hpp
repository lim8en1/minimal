#pragma once

// The model picker modal: a 2-tier (provider -> model) filterable menu over
// a ListModels snapshot. Same "modal over data it does not own" shape as
// MakeSessionPicker: the row building is pure (model_picker_model.cpp), the
// component is rebuilt in place by PickerController whenever the underlying
// list changes (refresh discovery), and selection just reports the chosen
// ref -- the switch itself re-runs the /model command through the normal
// dispatch path, server-side rules included.

#include <ftxui/component/component.hpp>

#include "minimal/client/sdk/theme.hpp"
#include "filterable_menu.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client::ui_kit {

// The /subagent-model picker's prepended row. Selection means "clear the
// session's subagent-model override -- spawned children run the parent's
// model". Carried as a row id so on_select can tell it from a real model ref.
inline constexpr const char* kSameAsParentRowId = "same-as-parent";
// The /model picker's prepended row: back to settings.json default_model (the
// session's own choice cleared).
inline constexpr const char* kDefaultModelRowId = "default";

struct ModelPickerCallbacks {
    std::function<void(const std::string& ref)> on_select;
    // ctrl+r: re-fetch the model list with discovery refreshed. The caller
    // owns the fetch (it can take seconds; every provider's catalog is
    // fetched in parallel server-side) and rebuilds the component in place.
    std::function<void()> on_refresh_discovery;
    std::function<void()> on_close;   // Escape
    // Prepends one selectable row above the model list (the /subagent-model
    // picker's "same as parent"). It survives filtering -- it is added in the
    // row builder, not to the model list -- and on_select receives
    // leading_row_id for it. Empty id = no leading row.
    std::string leading_row_label;
    std::string leading_row_id;
    // Offers the typed filter itself as a choice ("use 'x/y'") when it reads
    // as a `<provider>/<model>` ref and no listed model is exactly it -- a
    // ref the catalog does not list (discovery offline, a provider not
    // configured yet) stays choosable. on_select receives the typed text.
    bool allow_custom = false;
    // The window's title and the Enter hint ("switch model" / "switch").
    std::string window_title = "switch model";
    std::string select_hint = "switch";
};

ftxui::Component MakeModelPicker(const std::vector<v1::ModelSummary>& models,
                                 const Theme& theme, ModelPickerCallbacks callbacks,
    MenuStatus status = {});

}  // namespace minimal::client::ui_kit
