#include "windbg_session.hpp"

#include "minimal/sidecar/lifetime_watch.hpp"

namespace minimal::tools_windbg {

std::shared_ptr<WindbgSession> WindbgSessionStore::Find(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = sessions_.find(session_id); it != sessions_.end()) return it->second;
    return nullptr;
}

void WindbgSessionStore::Add(const std::string& session_id,
                             std::shared_ptr<WindbgSession> session) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[session_id] = std::move(session);
}

void WindbgSessionStore::CloseOnce(const WindbgSession& session, const char* reason) {
    bool expected = false;
    if (!session.closed->compare_exchange_strong(expected, true)) return;
    client_.Close(session.handle, on_exit_);
    (void)reason;  // the callers log context; the close itself is fire-and-forget
}

void WindbgSessionStore::Erase(const std::string& session_id, const char* reason) {
    std::shared_ptr<WindbgSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = sessions_.find(session_id); it != sessions_.end()) {
            session = it->second;
            sessions_.erase(it);
        }
    }
    if (!session) return;
    // Disarm the lifetime watcher BEFORE the explicit close: releasing the
    // guard makes its next poll bail, so the close below is the only one.
    session->lifetime_guard.reset();
    CloseOnce(*session, reason);
}

}  // namespace minimal::tools_windbg
