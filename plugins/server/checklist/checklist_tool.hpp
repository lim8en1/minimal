#pragma once

// The `checklist` tool: the model's task scratchpad for multi-step work.
//
// One tool, three ops, rather than three tools: the model addresses the same
// per-session state either way, and one spec keeps the system-prompt surface
// (and the permission key) a single name.
//
// Category ReadOnly, and that is not a stretch: the tool reaches no filesystem,
// no process, no network -- it writes this plugin's own storage namespace.
// Default permission policy for ReadOnly is Allow, which is the right posture
// for the model's own scratchpad; gating "the model updated its todo list"
// would only train the operator to click through.

#include <memory>

#include "checklist_store.hpp"
#include "minimal/plugins/plugin_abi.h"
#include "minimal/tools/tool.hpp"

namespace minimal::checklist {

std::unique_ptr<tools::Tool> MakeChecklistTool(std::shared_ptr<ChecklistStore> store);

}  // namespace minimal::checklist
