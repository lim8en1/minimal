#pragma once

// The /mcp picker modal: a flat filterable list over a ListMcpServers
// snapshot. Same "modal over data it does not own" shape as MakeModelPicker:
// row building is pure (mcp_picker_model.cpp), the component is rebuilt in
// place by PickerController whenever the list changes (a toggle), and
// selecting a row toggles that server's enabled flag live -- there is no
// "attach"/"switch" action here, a picker over a live enable/disable list has
// nothing else for Enter to mean.

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct McpPickerCallbacks {
    // Enter on a row: toggle that server's enabled flag. The caller owns the
    // SetMcpServerEnabled round trip and rebuilds the component in place
    // (mirrors ModelPickerCallbacks::on_refresh_discovery).
    std::function<void(const std::string& name)> on_toggle;
    std::function<void()> on_close;  // Escape
};

ftxui::Component MakeMcpPicker(const std::vector<v1::McpServerSummary>& servers,
                               const Theme& theme, McpPickerCallbacks callbacks);

}  // namespace minimal::client
