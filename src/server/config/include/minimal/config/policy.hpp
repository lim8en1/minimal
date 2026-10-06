#pragma once

// config policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::config {

// --- settings.json tools.* defaults (also LoopConfig's) ------------------------

// Tool calls from one assistant turn run concurrently up to this width: the
// common fan-out (a few reads, a grep) at once, without a turn monopolizing
// the machine. 1 reproduces sequential dispatch.
inline constexpr int kDefaultMaxParallelToolCalls = 4;

// One tool call's serialized input, and one result's text. Far past any real
// call or a useful result, small enough that a runaway model output or a
// misbehaving MCP server cannot ride into every later request until
// compaction.
inline constexpr std::int64_t kDefaultMaxToolInputBytes = 10 * common::kMiB;
inline constexpr std::int64_t kDefaultMaxToolResultBytes = 10 * common::kMiB;

// The pool starts one thread per slot, per session: past this a value is a
// resource-exhaustion typo, not a tuning choice.
inline constexpr int kMaxParallelToolCalls = 64;

// A result's client-only display payload (a diff, a table): past this it is
// dropped and the client shows the text. Room for a large diff; a display
// payload is never the record.
inline constexpr std::int64_t kDefaultMaxToolDisplayBytes = 128 * common::kKiB;

// --- settings.json memory.* ---------------------------------------------------

// Share of the working history a compaction replaces with the summary: most of
// it, so the window actually frees, while the newest part stays verbatim for
// the turn in progress.
inline constexpr int kDefaultCompactionReplacePercent = 80;

// --- settings.json tools_view / context_contributors ---------------------------

// Trailing stdout lines kept per running tool for the tools view: a screenful
// and some scrollback.
inline constexpr std::int64_t kDefaultToolsViewStdoutBufferLines = 100;

// One plugin context section's body: a few pages of instructions. A
// contributor that needs more declares its own cap.
inline constexpr std::int64_t kDefaultContextSectionMaxBytes = 16 * common::kKiB;

// --- settings.json sessions.* / server.* ----------------------------------------

// A quiescent detached session is reaped after this. Long enough to step away
// and re-attach to the live session; collecting it later is lossless anyway
// (re-attach becomes resume).
inline constexpr std::chrono::seconds kDefaultDetachedIdleTtl = std::chrono::minutes(30);

// A session no client ever attaches to is terminated after this: a safety net
// against a non-conforming client, far past a real client's create-then-attach.
inline constexpr std::chrono::seconds kDefaultInitialAttachGuard{10};

// The server exits this long after its last client and session are gone: long
// enough for a client restart to reconnect instead of respawning a server.
inline constexpr std::chrono::seconds kDefaultIdleShutdown{5};

// Budget for warming discovered model catalogs after startup (startup never
// waits on it): a reachable backend lists its models well within it.
inline constexpr std::chrono::seconds kDefaultDiscoveryWarmupTimeout{15};

// --- permissions.json / hooks.json --------------------------------------------

// The permission advisor's budget: long enough for a real policy decision,
// short enough that a wedged external decider turns into a human prompt
// within a minute.
inline constexpr std::chrono::milliseconds kDefaultAdvisorTimeout = std::chrono::seconds(30);

// A hook action's budget when hooks.json names none: room for a script that
// shells out to a linter or a quick API call; a hung hook is reported in
// seconds rather than stalling the turn.
inline constexpr std::chrono::milliseconds kDefaultHookTimeout = std::chrono::seconds(10);

}  // namespace minimal::config
