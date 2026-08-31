#pragma once

// The shell-action executor for hook actions (track O).
//
// Runs a hook action's command the same way the bash tool runs a model
// command -- `/bin/sh -c`, own process group, wall-clock deadline, bounded
// reaping -- but deliberately SIMPLER, because the trust story is different:
// a hook command is user config (the operator wrote it), not model input. No
// consent scoping, no session state, no cwd handling -- the payload carries
// `project_root` for scripts that want it. This file is core because the hook
// MECHANISM is core (plugins contribute hook points, not runners); it stays
// private to the hooks module and does not refactor tools-bash's richer
// wrapper, which exists for a different contract (fd-9 state trailer,
// incremental emit_output, session env).
//
// THE EXIT-CODE LADDER IS THE WHOLE CONTRACT:
//   exit 0            -> Pass
//   exit 2            -> Block, reason = first non-empty line of stdout/stderr
//   anything else     -> FailOpen (spawn failure, timeout, cancellation,
//                        signals, other codes): the emission behaves as if
//                        the action never ran, and the reason is logged.
// Fail-open is enforced by the CALLER (the dispatcher), not by the child
// process -- a broken hook degrades, never wedges, and never widens what the
// permission substrate already allowed.

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace minimal::hooks {

enum class ShellActionOutcome { kPass, kBlock, kFailOpen };

struct ShellActionResult {
    ShellActionOutcome outcome = ShellActionOutcome::kPass;
    std::string block_reason;  // set when kBlock
    std::string fail_reason;   // set when kFailOpen; logged, never surfaced to the model
};

// `timeout` <= 0 disables the wall-clock budget (cancellation still applies).
// Blocks the calling thread -- veto-capable emissions run on the session
// worker, which is the thread allowed to block; async emissions run on the
// dispatcher's own thread.
ShellActionResult RunShellAction(const std::string& command, std::string_view stdin_payload,
                                 std::chrono::milliseconds timeout,
                                 const std::function<bool()>& is_cancelled);

}  // namespace minimal::hooks
