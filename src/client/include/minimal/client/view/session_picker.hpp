#pragma once

// The session picker (T8): a modal over data it does not own -- refresh
// happens by rebuilding the component, same as before -- now with a
// "+ new session" row, a type-to-filter box, a recent/name sort toggle, and
// an include_all_dirs toggle. `d` moved to ctrl+d because a bare character
// is now the filter's alphabet.

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct SessionPickerCallbacks {
    std::function<void(const std::string& session_id)> on_resume;
    std::function<void(const std::string& session_id)> on_delete;
    std::function<void()> on_new;    // the synthetic "+ new session" row
    std::function<void()> on_close;
    // Filter and sort are pure client-side transforms the component applies
    // to `sessions` itself (picker_model.hpp) -- no callback needed. These
    // two DO need the host, because they change what the NEXT fetch should
    // ask the server for, and the component holds no client/AppState
    // reference (it is data in, callbacks out, like every other view here):
    std::function<void()> on_toggle_all_dirs;   // ctrl+a: refetch with include_all_dirs flipped
    std::function<void()> on_load_more;         // the highlighted row is the last one loaded
};

// `sessions` is the RAW accumulated list (server delivery order; "load more"
// appends further pages to it before the host rebuilds this component).
// `include_all_dirs`/`has_more` drive the hint line only -- the actual
// refetch behind either is the host's job (see the callbacks above).
ftxui::Component MakeSessionPicker(const std::vector<v1::SessionSummary>& sessions,
                                   bool include_all_dirs, bool has_more, const Theme& theme,
                                   SessionPickerCallbacks callbacks);

}  // namespace minimal::client
