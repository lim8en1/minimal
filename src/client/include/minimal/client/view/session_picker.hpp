#pragma once

// The session picker modal: a browser of PAST sessions (everything already
// visible as a tab is not listed twice), filtered to the current project_root
// by default, subagents excluded, sorted by last activity. Opening at startup
// and the delete-with-confirmation flow are RunTui's.

#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct SessionPickerCallbacks {
    std::function<void(const std::string& session_id)> on_resume;   // AttachTab
    std::function<void(const std::string& session_id)> on_delete;   // DeleteSession, with confirmation
    std::function<void()> on_close;
};

// `sessions` is the ListSessions response already fetched by the caller; the
// picker is a modal over data it does not own, so refresh happens by rebuilding.
ftxui::Component MakeSessionPicker(const std::vector<v1::SessionSummary>& sessions,
                                   const Theme& theme, SessionPickerCallbacks callbacks);

}  // namespace minimal::client
