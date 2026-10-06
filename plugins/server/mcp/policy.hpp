#pragma once

// mcp policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::mcp {

// --- plugins.json mcp.config defaults -------------------------------------------------

// Connect (spawn + initialize, or the http handshake): a local server is up in
// a second or two; a slow npx cold start fits; a dead one is reported.
inline constexpr std::chrono::milliseconds kDefaultConnectTimeout = std::chrono::seconds(10);
// One request: tool calls can do real work (a search, a build query).
inline constexpr std::chrono::milliseconds kDefaultRequestTimeout = std::chrono::minutes(1);
// tools/list pages followed (http): far past any real catalog; stops a server
// that paginates forever.
inline constexpr std::int64_t kDefaultMaxListPages = 100;
// Floor between restart attempts for a failed server: a crash loop is not
// respawned in a tight loop.
inline constexpr std::chrono::milliseconds kDefaultRestartCooldown = std::chrono::seconds(30);

// --- connection internals -------------------------------------------------------------

// A server verified alive is not re-pinged within this: a dedup for
// near-simultaneous session checks, not a liveness cache -- a server that died
// seconds ago must still be caught by the next check.
inline constexpr std::chrono::milliseconds kVerifiedTtl = std::chrono::seconds(2);
// One stdio line (a JSON-RPC message): far past any conforming message;
// an unterminated stream is a misbehaving child and the connection drops.
inline constexpr std::size_t kMaxStdioLineBytes = 16 * common::kMiB;
// Text content of one tool result before the agent's own result cap applies:
// keeps a hostile response from forcing a huge transient copy.
inline constexpr std::size_t kMaxToolResultText = common::kMiB;
// Shutdown: how long a worker gets to leave a call in flight before it is
// detached, and how long a stdio child gets to exit before SIGKILL.
inline constexpr std::chrono::milliseconds kWorkerJoinGrace{250};
inline constexpr std::chrono::milliseconds kChildTerminateGrace = std::chrono::seconds(5);
// The dead-connection reaper's backstop wakeup (a notify normally wakes it).
inline constexpr std::chrono::milliseconds kReaperBackstop{500};

}  // namespace minimal::mcp
