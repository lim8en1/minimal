#pragma once

#include <cstddef>

namespace minimal::common {

// Marks every open file descriptor above stdio close-on-exec, so a child
// spawned later (bash commands, MCP stdio servers, hook actions) inherits only
// the descriptors its spawner explicitly dups -- the server's gRPC, SQLite and
// log descriptors stop leaking into everything the harness runs.
//
// Call once at serve-stage start, after every long-lived descriptor is open and
// before the RPC server listens. Best-effort: a platform without a self-fd
// listing returns 0 and the pre-sweep inheritance behavior stands (the known
// gap this closes, DEVELOPMENT_PLAN.md §7).
//
// Returns the number of descriptors newly marked.
std::size_t MarkInheritedFdsCloseOnExec();

}  // namespace minimal::common
