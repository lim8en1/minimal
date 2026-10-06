#include "minimal/rpc/server.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>

#include <grpcpp/server_builder.h>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/units.hpp"

namespace minimal::rpc {
namespace {

// A socket file left behind by a crashed server would block every future bind.
// Removing one is safe only when nothing is listening on it, which connect()
// answers definitively -- an existence check cannot, and would happily delete a
// live server's socket out from under it.
bool SocketIsDead(const std::filesystem::path& path) {
    const std::string name = path.string();
    if (name.size() + 1 > sizeof(sockaddr_un{}.sun_path)) return false;

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", name.c_str());

    const bool connected =
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    const int connect_errno = errno;
    ::close(fd);

    if (connected) return false;                    // somebody is listening
    return connect_errno == ECONNREFUSED || connect_errno == ENOENT;
}

}  // namespace

std::filesystem::path DefaultSocketPath() { return common::DataDir() / "server.sock"; }

std::string SocketTarget(const std::filesystem::path& socket_path) {
    return "unix://" + socket_path.string();
}

Server::Server(SessionServiceImpl& service, std::filesystem::path socket_path,
               std::chrono::milliseconds shutdown_grace)
    : socket_path_(std::move(socket_path)), shutdown_grace_(shutdown_grace) {
    std::error_code ec;
    std::filesystem::create_directories(socket_path_.parent_path(), ec);

    if (std::filesystem::exists(socket_path_) && SocketIsDead(socket_path_)) {
        std::filesystem::remove(socket_path_, ec);
    }

    grpc::ServerBuilder builder;
    // EXPLICIT message limits, symmetric at 64 MiB to match the tool-output
    // cap every producer already applies. The gRPC defaults were asymmetric
    // and implicit: receive capped at 4 MB (a large tool_use input or a fat
    // rewind payload failed with an opaque RESOURCE_EXHAUSTED), send
    // unlimited (a runaway response was bounded by nothing on the wire).
    constexpr int kMaxMessageBytes = 64 * common::kMiB;
    builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
    builder.SetMaxSendMessageSize(kMaxMessageBytes);
    int selected_port = 0;
    builder.AddListeningPort(SocketTarget(socket_path_), grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    server_ = builder.BuildAndStart();  // null when the socket could not be bound
    if (server_ != nullptr) {
        // The socket IS the access control boundary (the topology is local and
        // single-user -- see server.hpp), so force it owner-only regardless of
        // the umask it was created under. Best-effort: a failure is logged, not
        // fatal, rather than refusing to serve over a socket that exists.
        if (::chmod(socket_path_.c_str(), 0700) != 0) {
            common::DefaultLogger().warn("could not restrict permissions on {}: {}",
                                         socket_path_.string(), std::strerror(errno));
        }
    }
}

Server::~Server() {
    Shutdown();
    // The socket file is ours only if we bound it; leaving it behind would make
    // the next start pay for a stale-socket probe that this can just avoid.
    if (server_ != nullptr) {
        std::error_code ec;
        std::filesystem::remove(socket_path_, ec);
    }
}

void Server::Wait() {
    if (server_ == nullptr) return;

    // How long in-flight RPCs get to finish once shutdown is requested, before
    // they are cancelled. grpc::Server::Shutdown(deadline) does exactly that:
    // graceful until the deadline, forced cancellation at it.
    const auto grace = shutdown_grace_;

    // The blocking Shutdown() runs here, on a thread that is not serving an RPC,
    // so a handler can ask for shutdown without waiting on its own completion.
    std::thread shutdown_watcher([this, grace] {
        std::unique_lock<std::mutex> lock(mutex_);
        shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
        // THE DEADLINE. An unbounded graceful shutdown let a half-open stream --
        // a client whose process died without closing its socket -- block
        // Wait() and therefore process exit indefinitely: the stream errors
        // only when something is left to error it. In-flight work gets the
        // grace window; past it, the RPCs (and their sessions) are cancelled
        // and the process exits.
        const auto deadline = std::chrono::system_clock::now() + grace;
        // Released while Shutdown blocks, for the same reason the blocking
        // work lives on this thread at all: RequestShutdown() takes this
        // mutex, and a concurrent caller of it must not wait out the drain.
        lock.unlock();
        server_->Shutdown(deadline);
        lock.lock();
        // This wait ends at roughly the same moment grpc's deadline cancel
        // fires, so the log lands when the hard shutdown engages -- skipped
        // entirely when everything drained inside the grace.
        if (shutdown_cv_.wait_for(lock, grace, [this] { return server_stopped_; })) {
            return;
        }
        common::DefaultLogger().warn(
            "shutdown grace ({} ms) expired with RPCs still in flight; cancelling them",
            grace.count());
    });

    server_->Wait();
    RequestShutdown();  // in case Wait() returned for some other reason
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_stopped_ = true;
    }
    shutdown_cv_.notify_all();
    shutdown_watcher.join();
}

void Server::RequestShutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_requested_ = true;
    }
    shutdown_cv_.notify_all();
}

void Server::Shutdown() {
    RequestShutdown();
    if (server_ != nullptr) {
        // Deadline-bounded even here. The deadline-free overload waits for
        // EVERY in-flight RPC to drain, and this runs from ~Server(): a
        // flow-control-blocked Write to a deaf client -- the same wedge the
        // Wait() path guards against with a grace deadline -- would hang the
        // destructor (and therefore process exit) indefinitely. In-flight
        // work gets the same grace it gets on the Wait() path; past it, the
        // cancellation fires.
        server_->Shutdown(std::chrono::system_clock::now() + shutdown_grace_);
    }
}

}  // namespace minimal::rpc
