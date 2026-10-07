#pragma once

// Core's model selector (the widget kit's ModelSelector): MakeModelPicker
// plus the fetch it lacks. The list comes from `spec.fetch`, run on the
// selector's own TaskWorker; while it is out the picker says "loading", a
// failure shows in place of the list, and ctrl+r fetches again with
// discovery refreshed -- the typed filter and the cursor kept across each
// rebuild.

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/widget_types.hpp"

namespace minimal::client::ui_kit {

// `theme` must outlive the component (the view's own theme member).
ftxui::Component MakeModelSelector(ModelSelectorSpec spec, ModelSelectorCallbacks callbacks,
                                   const Theme& theme);

}  // namespace minimal::client::ui_kit
