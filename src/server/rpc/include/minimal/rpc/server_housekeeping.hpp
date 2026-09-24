#pragma once

// The server's own periodic duties, on one thread: the idle exit and the
// session database's WAL checkpoint. Kept out of SessionRegistry, whose
// reaper is about sessions; this is about the process.
//
// THE IDLE EXIT. Once zero clients are attached, no live client has pinged
// recently, and no session holds the process up (SessionRegistry::
// AnySessionHolds), on_idle_expired fires after idle_shutdown of that state.
// A detached-but-busy session keeps the process up until its work finishes;
// a live session-less client (the TUI's welcome view) keeps it up by pinging.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

#include "minimal/common/periodic_task.hpp"

namespace minimal::rpc {

class ServerHousekeeping {
public:
    struct Config {
        // 0 = never exit (daemon deployment).
        std::chrono::seconds idle_shutdown = std::chrono::seconds(5);
        // The service's attached-stream count. Empty disables the idle exit.
        std::function<std::size_t()> attached_clients;
        // Age of the most recent client Ping. The exit holds while this is
        // below idle_shutdown. Empty: only the count and sessions decide.
        std::function<std::chrono::milliseconds()> client_idle_for;
        // SessionRegistry::AnySessionHolds. Empty: sessions never hold.
        std::function<bool()> sessions_hold;
        // Fired from the housekeeping thread; must not block (production:
        // Server::RequestShutdown). Empty disables the idle exit.
        std::function<void()> on_idle_expired;
        // Called every tick, after the idle check. Empty: no periodic work.
        std::function<void()> periodic_checkpoint;
        std::chrono::milliseconds tick = std::chrono::milliseconds(250);
    };

    // Starts ticking immediately.
    explicit ServerHousekeeping(Config config);
    ~ServerHousekeeping();

    ServerHousekeeping(const ServerHousekeeping&) = delete;
    ServerHousekeeping& operator=(const ServerHousekeeping&) = delete;

    // Idempotent. After it returns no callback runs again -- call it before
    // tearing down anything the callbacks capture.
    void Stop();

    // One tick at `now`. Public so tests can drive the clock.
    void Tick(std::chrono::steady_clock::time_point now);

private:
    Config config_;
    std::optional<std::chrono::steady_clock::time_point> idle_since_;
    std::unique_ptr<common::PeriodicTask> task_;  // last: its thread reads the above
};

}  // namespace minimal::rpc
