#include "minimal/client/server_spawn.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include <grpcpp/create_channel.h>

#include "minimal/common/bundled_assets.hpp"
#include "minimal/common/config_paths.hpp"

namespace minimal::client {
namespace {

// The server socket path a unix:// target names -- the same string --listen
// takes (rpc::SocketTarget is this function's inverse).
std::optional<std::filesystem::path> SocketPathOf(const std::string& target) {
    constexpr std::string_view scheme = "unix://";
    if (target.rfind(scheme, 0) != 0) return std::nullopt;
    const std::string path = target.substr(scheme.size());
    if (path.empty()) return std::nullopt;
    return std::filesystem::path(path);
}

bool ExecutableFile(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    const auto perms = std::filesystem::status(path, ec).permissions();
    if (ec) return false;
    return (perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none;
}

// Double fork: the intermediate child exits immediately (the client's waitpid
// below never blocks), the real server is orphaned into init's care and holds
// no controlling terminal. Its exit is therefore never observable from here --
// startup diagnostics live in the log, which is why the failure path names it.
bool SpawnDetached(const std::filesystem::path& binary,
                   const std::filesystem::path& socket_path, const std::string& log_path) {
    const pid_t middle = ::fork();
    if (middle < 0) return false;
    if (middle == 0) {
        ::signal(SIGHUP, SIG_IGN);
        if (::setsid() < 0) _exit(126);
        const pid_t server = ::fork();
        if (server < 0) _exit(126);
        if (server == 0) {
            const int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (log_fd >= 0) {
                ::dup2(log_fd, STDOUT_FILENO);
                ::dup2(log_fd, STDERR_FILENO);
                if (log_fd > STDERR_FILENO) ::close(log_fd);
            }
            const int null_fd = ::open("/dev/null", O_RDONLY);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDIN_FILENO);
                if (null_fd != STDIN_FILENO) ::close(null_fd);
            }
            ::chdir("/");
            ::execl(binary.c_str(), binary.filename().c_str(), "--listen",
                    socket_path.c_str(), static_cast<char*>(nullptr));
            _exit(127);  // execl failed; the probe timeout reports it
        }
        _exit(0);
    }
    int status = 0;
    while (::waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }
    return true;
}

// A probe channel of our own: the caller's SessionClient may not exist yet, and
// its channel would connect lazily anyway. Reachable == listening, which is
// what "the server is up" means here (the bind happens after plugin load and
// the sanity check; anything earlier simply is not on the socket yet).
bool Reachable(const std::string& target, std::chrono::milliseconds timeout) {
    const auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    return channel->WaitForConnected(std::chrono::system_clock::now() + timeout);
}

}  // namespace

bool IsSpawnableTarget(const std::string& target) {
    return SocketPathOf(target).has_value();
}

std::string SpawnedServerLogPath() { return (common::DataDir() / "server-spawn.log").string(); }

std::string LocateServerBinary() {
    if (const char* override_bin = std::getenv("MINIMAL_SERVER_BIN");
        override_bin != nullptr && *override_bin != '\0') {
        return override_bin;
    }

    // Installed layout: <prefix>/bin holds both binaries side by side.
    const auto exe = common::ExecutablePath();
    if (!exe.empty()) {
        const auto sibling = exe.parent_path() / "minimal-server";
        if (ExecutableFile(sibling)) return sibling.string();

        // Build tree: the client sits at <build>/src/client, so three hops up
        // from the executable reach the build root, where the server binary
        // sits beside the plugins it loads -- the same shape BundledPluginDir's
        // probe uses.
        const auto staged = exe.parent_path().parent_path().parent_path() / "src/server" /
                            "minimal-server";
        if (ExecutableFile(staged)) return staged.string();
    }
    return {};
}

bool EnsureServerRunning(const std::string& target, const SpawnOptions& options,
                         const std::function<void(const std::string&)>& progress) {
    const auto say = [&progress](const std::string& line) {
        if (progress) progress(line);
    };

    // First probe, with a short timeout: the common case is a server already
    // running, and no spawn machinery should run on its behalf.
    if (Reachable(target, std::chrono::milliseconds(2000))) return true;
    if (!options.enabled) return false;
    if (!IsSpawnableTarget(target)) return false;

    const auto socket_path = SocketPathOf(target);
    const std::string binary = LocateServerBinary();
    if (binary.empty() || !socket_path) return false;

    say("no server at " + target + "; starting " + binary);
    // The log's parent has to exist before the grandchild's open(O_CREAT):
    // DataDir() is where the server would create it anyway, but the redirect
    // happens first -- a missing directory would silently swallow the child's
    // diagnostics entirely.
    {
        std::error_code ec;
        std::filesystem::create_directories(common::DataDir(), ec);
    }
    if (!SpawnDetached(binary, *socket_path, SpawnedServerLogPath())) {
        say("failed to spawn minimal-server");
        return false;
    }

    // The socket appears only once the server has finished config load, plugin
    // load and the sanity check -- give that the full budget.
    const auto deadline = std::chrono::steady_clock::now() + options.startup_timeout;
    for (;;) {
        if (Reachable(target, std::chrono::milliseconds(500))) return true;
        if (std::chrono::steady_clock::now() >= deadline) break;
    }

    say("spawned minimal-server did not become reachable within " +
        std::to_string(std::chrono::milliseconds(options.startup_timeout).count() / 1000) +
        "s; see " + SpawnedServerLogPath());
    return false;
}

}  // namespace minimal::client
