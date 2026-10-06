#pragma once

#include <memory>

#include "minimal/common/live_value.hpp"
#include "minimal/hooks/hook_dispatcher.hpp"
#include "minimal/tools/tool.hpp"

#include "bash_session_state.hpp"
#include "background_registry.hpp"
#include "bash_settings.hpp"

namespace minimal::tools_bash {

// The bash tool. `store` is plugin-owned (see BashSessionStateStore); the tool
// holds a reference because it and the future /restart-shell command and
// SessionObserver all operate on the same map -- one .so, one state owner.
// `background` is the same plugin's background task registry: the state the
// tool's detach path (run_in_background is a kept alias) creates and the
// /tasks command lists.
//
// `before_command` is this plugin's OWN hook point (veto-capable): a hook
// action may refuse an individual invocation before it spawns, at a finer
// grain than core's pre_tool_use because the payload carries the RESOLVED
// program path (disclosure-time resolution, session PATH included). Default
// (disabled) for tests that build the tool directly.
//
// `live` is read once per call: a reconfigure swaps it under a running
// server. The settings overload is a fixed configuration (tests).
std::unique_ptr<tools::Tool> MakeBashTool(
    std::shared_ptr<const common::LiveValue<BashSettings>> live, BashSessionStateStore& store,
    BackgroundTaskRegistry& background, hooks::HookPoint before_command = hooks::HookPoint{});
std::unique_ptr<tools::Tool> MakeBashTool(
    BashSettings settings, BashSessionStateStore& store,
    BackgroundTaskRegistry& background,
    hooks::HookPoint before_command = hooks::HookPoint{});

// The point this plugin registers, under the plugin-qualified convention
// shared with services. Declared here so a consumer (or a test) can address
// the name without hardcoding it.
inline constexpr char kBeforeCommandPoint[] = "tools-bash.before_command";

}  // namespace minimal::tools_bash
