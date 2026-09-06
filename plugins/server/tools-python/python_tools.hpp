#pragma once

#include <memory>

#include "minimal/tools/tool.hpp"

#include "python_settings.hpp"
#include "python_store.hpp"

namespace minimal::tools_python {

// python_exec -- run code in this session's persistent interpreter.
//
//   timeout = 0 (default): async. Acknowledges immediately ("py-N (pid P)");
//     the result is INJECTED at a turn boundary keyed to the original
//     tool_use_id when it completes. There is NO async deadline: the request
//     finishes when it finishes, and the injection waits. This is the mode
//     for long-running work (a blocked MSRPC call, a long download) and for
//     fire-and-forget setup.
//   timeout > 0: sync. Blocks up to min(timeout, sync_wait_ceiling); on
//     expiry (or CancelTurn) the request DEMOTES to async -- the interpreter
//     and its state are untouched, and the result still arrives by injection.
//     Timeouts demote, never destroy.
//   clean = true: SIGKILL the current interpreter first, then run in a fresh
//     one (also the recovery path for a wedged interpreter: state is lost as
//     an explicit choice).
//
// A fresh interpreter is started on first use. The environment is PER-PROJECT
// and shared by every agent in it: <project_root>/.venv, created on demand
// when the preset declares packages/requirements. The preset's DECLARED
// package contract is diffed against the interpreter's on EVERY call:
// additive entries are auto-installed; spec conflicts are reported loudly,
// once, and left to the model.
std::unique_ptr<tools::Tool> MakePythonExecTool(PythonSettings settings,
                                                InterpreterStore& store);

// python_interrupt -- stop the currently-running request, or the interpreter.
//
//   kill = false (default): soft. Out-of-band SIGINT (never through the
//     serialized queue -- a stuck driver cannot read one). The running
//     request fails with KeyboardInterrupt; the interpreter and its state
//     survive. Reports honestly whether it landed.
//   kill = true: hard. SIGKILL + reap. Pending requests get injected error
//     results; the next python_exec starts a fresh interpreter. State is
//     lost as an explicit choice.
std::unique_ptr<tools::Tool> MakePythonInterruptTool(PythonSettings settings,
                                                     InterpreterStore& store);

}  // namespace minimal::tools_python
