#pragma once

#include <memory>

#include "minimal/common/live_value.hpp"
#include "minimal/tools/tool.hpp"

#include "python_settings.hpp"
#include "python_store.hpp"

namespace minimal::tools_python {

// python_exec -- run code in this session's persistent interpreter.
//
//   timeout = 0 (default): async. Acknowledges immediately ("py-N started");
//     the result is INJECTED at a turn boundary keyed to the original
//     tool_use_id -- it arrives as a tool result before the model's next
//     reply. python_output polls progress and partial stdout meanwhile.
//   timeout > 0: sync. Blocks up to min(timeout, sync_wait_ceiling); on
//     expiry (or CancelTurn) the request DEMOTES to async -- the interpreter
//     and its state are untouched, and the result still arrives by injection.
//     Timeouts demote, never destroy.
//   max_runtime (seconds, default unlimited): the request's own deadline,
//     counted from when it starts RUNNING (queue time excluded). On expiry
//     the request is soft-interrupted; if it ignores SIGINT for
//     grace_seconds, the interpreter is hard-killed (state LOST). The one
//     destructive timer, and it is opt-in.
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
//
// `live` is read per call: sync_wait_ceiling_s and background_max_runtime_s
// apply from the next call after a reconfigure. Everything an interpreter
// copies at spawn stays restart. The settings overload is fixed (tests).
std::unique_ptr<tools::Tool> MakePythonExecTool(
    std::shared_ptr<const common::LiveValue<PythonSettings>> live, InterpreterStore& store);
std::unique_ptr<tools::Tool> MakePythonExecTool(PythonSettings settings,
                                                InterpreterStore& store);

// python_output -- poll background work without blocking.
//
//   No arguments (or scope="all"): one status line per task -- running (with
//     age and live-stream size), queued behind N, or finished (ok/failed,
//     total runtime; the result arrives with the next turn if not already
//     delivered).
//   task_id + scope: that task's output. "top:N" / "tail:N" / "read:N:M"
//     slice the LOGICAL stream by absolute byte offsets (a running task's
//     live ring; a finished task's capped capture). Responses carry the
//     stream total and next_offset for paging; bytes that fell out of the
//     running ring come back as explicit gap markers.
//   grep (optional regex): filters the scoped slice's lines, matches
//     carrying their byte offsets within the slice.
std::unique_ptr<tools::Tool> MakePythonOutputTool(PythonSettings settings,
                                                  InterpreterStore& store);

// python_input -- feed the stdin channel of the running code.
//
// Executed code's sys.stdin reads a dedicated pipe: a blocking input() parks
// the interpreter until python_input feeds a line (a newline is appended when
// absent). Interruptible like any blocking call (python_interrupt).
std::unique_ptr<tools::Tool> MakePythonInputTool(PythonSettings settings,
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
