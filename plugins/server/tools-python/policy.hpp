#pragma once

// tools-python policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::tools_python {

// --- plugins.json python.config defaults (seconds, per this plugin's schema) -----

// A sync python_exec wait that reaches this demotes to async (never kills):
// long enough for a real computation, short enough not to park the session.
inline constexpr std::int64_t kDefaultSyncWaitCeilingSeconds = 300;
// Spawn + handshake of a new interpreter: a healthy venv is up in a second or
// two; a broken one is caught here, not mid-use.
inline constexpr std::int64_t kDefaultStartupTimeoutSeconds = 10;
// venv creation and package installation: a cold pip install of a few
// packages over a slow network.
inline constexpr std::int64_t kDefaultInstallTimeoutSeconds = 300;
// What is persisted per request (sized for a context window: ~16-32k tokens
// of dense output; past it the whole text spills to a file the model can
// read), and the per-stream capture inside the interpreter (which also sizes
// the protocol line, and bounds what a spill can hold).
inline constexpr std::int64_t kDefaultMaxOutputBytes = 64 * static_cast<std::int64_t>(common::kKiB);
inline constexpr std::int64_t kDefaultMaxCaptureBytes = static_cast<std::int64_t>(common::kMiB);
// The live-output ring python_output polls: a head to see how it started, a
// longer tail to see where it is.
inline constexpr std::int64_t kDefaultStreamHeadBytes = 256 * static_cast<std::int64_t>(common::kKiB);
inline constexpr std::int64_t kDefaultStreamTailBytes = 768 * static_cast<std::int64_t>(common::kKiB);

// --- the interpreter process ---------------------------------------------------------

// The reader's poll slice: only the backstop for noticing a session's death
// (a kill or a request wakes poll() directly).
inline constexpr std::chrono::milliseconds kReaderPollSlice{500};
// Interpreter diagnostics (its stderr) kept for error reports.
inline constexpr std::size_t kMaxDiagBytes = 64 * common::kKiB;
// How long a response waits for live-stream bytes the driver says it wrote
// before the response line; only a miscounting driver ever waits this long.
inline constexpr std::chrono::milliseconds kStreamOwedBudget{500};
// The protocol line's floor, and the slack around the capture-derived size
// (a truncation marker per field, the JSON envelope).
inline constexpr std::size_t kMinProtocolLineBytes = 64 * common::kMiB;
inline constexpr std::size_t kProtocolFieldSlackChars = 64;
inline constexpr std::size_t kProtocolEnvelopeBytes = 64 * common::kKiB;
// Completed background requests python_output can still see.
inline constexpr std::size_t kFinishedRing = 8;

// --- tool surface ------------------------------------------------------------------

// One python_exec's code: far past any real snippet; a file is run by path.
inline constexpr std::size_t kMaxCodeBytes = common::kMiB;
// First-line previews of the code: a task's label, and a call's row header.
inline constexpr std::size_t kTaskLabelChars = 80;
inline constexpr std::size_t kSummaryChars = 200;
// python_output's tail when the model names no scope, and per task in a list.
inline constexpr std::size_t kDefaultOutputTailBytes = 8 * common::kKiB;
inline constexpr std::size_t kListOutputTailBytes = 2 * common::kKiB;

}  // namespace minimal::tools_python
