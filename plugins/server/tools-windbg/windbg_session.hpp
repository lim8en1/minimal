#pragma once

// The windbg session store: one dbgd session per harness session, keyed by
// session id -- the same per-session lifetime shape as the bash state store
// and the python interpreter store. What is special here is that the session
// lives OUTSIDE this process: it must also die when the harness session
// dies, and there is no synchronous kill hook at teardown
// (SessionObserver::OnSessionEnded is fire-and-forget). Two mechanisms cover
// it, both idempotent:
//   - the store's Erase (called from the observer) closes the remote session
//     explicitly and disarms the watcher;
//   - a sidecar WatchLifetime guard (held by each stored session) fires the
//     same close when ToolContext::session_lifetime expires first.

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "dbgd_client.hpp"

namespace minimal::tools_windbg {

// One live remote debugger session.
struct WindbgSession {
    std::string handle;        // dbgd's opaque session handle
    std::string target_text;   // the spec's describe() text, for recovery hints
    // The remote teardown, armed at creation. Held via shared_ptr<void> so
    // the store can drop it without naming the guard type.
    std::shared_ptr<void> lifetime_guard;
    // Closes the remote session exactly once (the observer, the lifetime
    // watcher, and an explicit restart all race the same close).
    std::shared_ptr<std::atomic<bool>> closed;
};

// Process-lifetime (plugins are never dlclosed), one per plugin.
class WindbgSessionStore {
public:
    WindbgSessionStore(DbgdClient& client, std::string on_exit)
        : client_(client), on_exit_(std::move(on_exit)) {}

    std::shared_ptr<WindbgSession> Find(const std::string& session_id);

    // Takes ownership of an already-created session (the tool builds it so a
    // connect failure never enters the store).
    void Add(const std::string& session_id, std::shared_ptr<WindbgSession> session);

    // Session teardown. Closes the remote session (idempotent) and drops the
    // record. The lifetime guard is released first, so the watcher never
    // double-fires.
    void Erase(const std::string& session_id, const char* reason);

private:
    void CloseOnce(const WindbgSession& session, const char* reason);

    DbgdClient& client_;
    std::string on_exit_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<WindbgSession>> sessions_;
};

}  // namespace minimal::tools_windbg
