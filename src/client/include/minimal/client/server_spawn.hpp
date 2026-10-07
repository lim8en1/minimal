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
#include <optional>
#include <string>

#include <grpcpp/support/status.h>

#include "minimal/client/policy.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::client {

struct SpawnOptions {
    bool enabled = true;  // --no-autospawn clears this
    std::chrono::milliseconds startup_timeout = kServerStartupTimeout;
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
// empty. Returns whether the target is reachable on return. `spawned`, when
// given, is set to whether a server was started (a failure after a spawn has
// already been explained through `progress`).
bool EnsureServerRunning(const std::string& target, const SpawnOptions& options,
                         const std::function<void(const std::string&)>& progress = {},
                         bool* spawned = nullptr);

// Whether the server that answered GetServerInfo with (status, info) is a
// different build from this client. nullopt = the same build; otherwise the
// multi-line diagnostic to print, naming both builds and how to stop the old
// server. Compares abi_tag (toolchain + git revision), not the release version:
// the stale server that matters is the one left running across a rebuild at
// the same release. UNIMPLEMENTED -- a server older than the RPC -- is stale
// by definition; any other failure is reported as "could not verify".
std::optional<std::string> ServerBuildMismatch(const grpc::Status& status,
                                               const v1::ServerInfo& info);

// The log a spawned server's stdout/stderr lands in.
std::string SpawnedServerLogPath();

}  // namespace minimal::client
