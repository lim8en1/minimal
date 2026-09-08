#pragma once

#include <memory>

#include "minimal/plugins/plugin_abi.h"
#include "minimal/sidecar/task_registry.hpp"

#include "dbgd_client.hpp"
#include "windbg_session.hpp"
#include "windbg_settings.hpp"

namespace minimal::tools_windbg {

// tools: windbg (exec, sync-with-demote or async) and windbg_interrupt. Both
// operate on the per-harness-session dbgd session the store owns; `windbg`
// connects on first use (from the call's `target` or the config's
// default_target) and every later call rides that session.
std::unique_ptr<tools::Tool> MakeWindbgTool(WindbgSettings settings, DbgdClient& client,
                                            WindbgSessionStore& store,
                                            sidecar::TaskRegistry& jobs);
std::unique_ptr<tools::Tool> MakeWindbgInterruptTool(WindbgSettings settings, DbgdClient& client,
                                                     WindbgSessionStore& store);

}  // namespace minimal::tools_windbg
