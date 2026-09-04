#pragma once

// Client-side auto-spawn: connect-or-start for the local server.
//
// The default topology is one server per user, started implicitly by the first
// client that needs it and torn down by the server's own idle reaper once no
// client, session, or heartbeat remains. This module owns the first half: when
// nothing is listening on a unix-socket target, spawn `minimal-server` detached
// and wait for the socket to come up.
//
// Deliberately narrow:
//   * Unix-socket targets ONLY. An explicit --server to a remote address is
//     outside the local-only trust model (ARCHITECTURE.md, Distribution); it
//     is the user's job to start that server.
//   * The spawned server is a peer of one started by hand, not a child the
//     client manages: double-fork + setsid detaches it fully, and its lifetime
//     is governed by the same reaper settings as any other server.
//   * Startup failure is diagnosed by the probe timing out, never by reading
//     the child's exit -- the double-fork means the spawner cannot see it.
//     The log path in the failure message is the intended diagnostic.

#include <chrono>
#include <functional>
#include <string>

namespace minimal::client {

struct SpawnOptions {
    bool enabled = true;  // --no-autospawn clears this
    std::chrono::milliseconds startup_timeout{25000};
};

// True when `target` names a filesystem socket this module could spawn a server
// for (the unix:// scheme -- exactly what the server's --listen accepts).
bool IsSpawnableTarget(const std::string& target);

// Where EnsureServerRunning would exec. Order: $MINIMAL_SERVER_BIN, then the
// client's own directory (installed layout), then the build-tree layout.
// Empty when nothing usable was found.
std::string LocateServerBinary();

// Ensures something is listening on `target`. Spawns a detached server when
// enabled, the target is spawnable, and no server binary was overridden away.
// `progress` receives human-readable status lines (stderr material); it may be
// empty. Returns whether the target is reachable on return.
bool EnsureServerRunning(const std::string& target, const SpawnOptions& options,
                         const std::function<void(const std::string&)>& progress = {});

// The log a spawned server's stdout/stderr lands in.
std::string SpawnedServerLogPath();

}  // namespace minimal::client
