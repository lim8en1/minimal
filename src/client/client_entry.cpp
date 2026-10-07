#include "client_entry.hpp"

#include <signal.h>

#include <chrono>
#include <cstdio>

#include "minimal/client/server_spawn.hpp"
#include "minimal/client/policy.hpp"
#include "minimal/common/config_paths.hpp"

namespace minimal::client::app {

SessionClient::CreateOptions DefaultCreateOptions() {
    SessionClient::CreateOptions opts;
    opts.project_root = CurrentProjectRootOrEmpty();
    return opts;
}

std::shared_ptr<SessionClient> ConnectOrReport(const std::string& target, bool autospawn,
                                               bool allow_version_mismatch) {
    SpawnOptions spawn_options;
    spawn_options.enabled = autospawn;
    bool spawned = false;
    const bool reachable = EnsureServerRunning(
        target, spawn_options,
        [](const std::string& line) { std::fprintf(stderr, "minimal: %s\n", line.c_str()); },
        &spawned);
    auto client = std::make_shared<SessionClient>(target);
    if (reachable && client->WaitForConnected(kChannelConnectTimeout)) {
        const auto info = client->GetServerInfo();
        const auto check = CheckServerBuild(info.status, info.info, target, spawned);
        using Verdict = ServerBuildCheck::Verdict;
        if (check.verdict == Verdict::kSame) return client;
        std::fprintf(stderr, "minimal: %s\n", check.message.c_str());
        if (check.verdict == Verdict::kUnverified) return client;
        if (allow_version_mismatch) {
            std::fprintf(stderr, "minimal: continuing anyway (--allow-version-mismatch)\n");
            return client;
        }
        // The server this client just started is of no use to anyone: left
        // running it would only answer the next run the same way.
        if (spawned && info.status.ok() && info.info.pid() > 0) {
            ::kill(static_cast<pid_t>(info.info.pid()), SIGTERM);
        }
        std::fprintf(stderr, "(or pass --allow-version-mismatch to connect regardless)\n");
        return nullptr;
    }
    // A spawned server that failed has already said why (its fatal lines, or
    // the log to read); "start one yourself" would only bury that.
    if (spawned && !reachable) return nullptr;
    std::fprintf(stderr, "cannot reach a minimal-server at %s\n", target.c_str());
    std::fprintf(stderr, "start one with: minimal-server --listen %s\n",
                 target.starts_with(common::kUnixSocketScheme)
                     ? target.c_str() + common::kUnixSocketScheme.size()
                                                 : "<socket path>");
    return nullptr;
}

}  // namespace minimal::client::app
