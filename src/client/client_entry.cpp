#include "client_entry.hpp"

#include <chrono>
#include <cstdio>

#include "minimal/client/server_spawn.hpp"

namespace minimal::client::app {

SessionClient::CreateOptions DefaultCreateOptions() {
    SessionClient::CreateOptions opts;
    opts.project_root = CurrentProjectRootOrEmpty();
    return opts;
}

std::shared_ptr<SessionClient> ConnectOrReport(const std::string& target, bool autospawn) {
    SpawnOptions spawn_options;
    spawn_options.enabled = autospawn;
    bool spawned = false;
    const bool reachable = EnsureServerRunning(
        target, spawn_options,
        [](const std::string& line) { std::fprintf(stderr, "minimal: %s\n", line.c_str()); },
        &spawned);
    auto client = std::make_shared<SessionClient>(target);
    if (reachable && client->WaitForConnected(std::chrono::seconds(5))) return client;
    // A spawned server that failed has already said why (its fatal lines, or
    // the log to read); "start one yourself" would only bury that.
    if (spawned && !reachable) return nullptr;
    std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
    std::fprintf(stderr, "start one with: minimal-server --listen %s\n",
                 target.rfind("unix://", 0) == 0 ? target.c_str() + 7 : "<socket path>");
    return nullptr;
}

}  // namespace minimal::client::app
