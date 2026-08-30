#pragma once

#include <memory>

#include "minimal/tools/tool.hpp"

#include "bash_session_state.hpp"
#include "bash_settings.hpp"

namespace minimal::tools_bash {

// The bash tool. `store` is plugin-owned (see BashSessionStateStore); the tool
// holds a reference because it and the future /restart-shell command and
// SessionObserver all operate on the same map -- one .so, one state owner.
std::unique_ptr<tools::Tool> MakeBashTool(BashSettings settings, BashSessionStateStore& store);

}  // namespace minimal::tools_bash
