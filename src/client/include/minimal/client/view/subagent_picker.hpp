#pragma once

// The /subagents picker: a MakeFilterableMenuModal wrapper over
// subagent_picker_model's rows -- the session_picker.cpp shape, minus
// everything a small, whole-fetched, one-parent list doesn't need (no
// pagination, no sort toggle, no synthetic rows). ctrl+a toggles whether
// ended children are listed; enter opens the highlighted child.

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/filterable_menu.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct SubagentPickerCallbacks {
    // `is_live` is the row's own summary flag, resolved before the host
    // round trip -- the selection routes attach (live) / transcript view
    // (ended) in PickerController.
    std::function<void(const std::string& session_id, bool is_live)> on_select;
    std::function<void()> on_close;
    // ctrl+a: flip whether ended children are listed. The list was fetched
    // whole, so this is a pure display toggle (rebuild), no re-fetch.
    std::function<void()> on_toggle_ended;
};

ftxui::Component MakeSubagentPicker(const std::vector<v1::SessionSummary>& children,
                                    bool include_ended, const Theme& theme,
                                    SubagentPickerCallbacks callbacks,
    MenuStatus status = {});

}  // namespace minimal::client
