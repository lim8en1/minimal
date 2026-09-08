#pragma once

// One detached watcher that fires a cleanup exactly once when a session's
// lifetime expires -- the mechanism behind "a tool's remote resources die
// with its session".
//
// Background: there is no synchronous plugin kill hook at teardown
// (SessionObserver::OnSessionEnded is dispatched fire-and-forget), so anything
// a tool owns OUTSIDE this process -- a remote debugger session, a spawned
// daemon -- cannot rely on teardown ordering. The pattern the in-process tools
// use is a poll: the watcher checks ToolContext::session_lifetime (which
// expires with the session) once per slice and self-terminates on expiry,
// with at most one slice of lag. This helper extracts that poll for tools
// whose cleanup cannot run inside a per-task body.
//
// Ownership: the returned guard is held by the object that owns the remote
// resource. Destroying the guard (explicit teardown, store erase) cancels the
// watcher BEFORE it fires; the lifetime expiring fires `on_expired` exactly
// once and then the watcher ends. `on_expired` must be safe to call from a
// detached thread and must not throw.

#include <chrono>
#include <functional>
#include <memory>

namespace minimal::sidecar {

// Returns a guard token the caller holds for as long as the cleanup should
// stay armed. `slice` is the poll granularity (the default matches the
// in-process tools' 100 ms expiry-lag budget).
std::shared_ptr<void> WatchLifetime(
    std::weak_ptr<const void> lifetime, std::function<void()> on_expired,
    std::chrono::milliseconds slice = std::chrono::milliseconds(100));

}  // namespace minimal::sidecar
