#include "minimal/rpc/server.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

#include <grpcpp/server_builder.h>

#include "minimal/common/config_paths.hpp"

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

Server::Server(SessionServiceImpl& service, std::filesystem::path socket_path)
    : socket_path_(std::move(socket_path)) {
    std::error_code ec;
    std::filesystem::create_directories(socket_path_.parent_path(), ec);

    if (std::filesystem::exists(socket_path_) && SocketIsDead(socket_path_)) {
        std::filesystem::remove(socket_path_, ec);
    }

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort(SocketTarget(socket_path_), grpc::InsecureServerCredentials(),
                             &selected_port);
    builder.RegisterService(&service);
    server_ = builder.BuildAndStart();  // null when the socket could not be bound
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

    // The blocking Shutdown() runs here, on a thread that is not serving an RPC,
    // so a handler can ask for shutdown without waiting on its own completion.
    std::thread shutdown_watcher([this] {
        std::unique_lock<std::mutex> lock(mutex_);
        shutdown_cv_.wait(lock, [this] { return shutdown_requested_; });
        lock.unlock();
        server_->Shutdown();
    });

    server_->Wait();
    RequestShutdown();  // in case Wait() returned for some other reason
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
    if (server_ != nullptr) server_->Shutdown();
}

}  // namespace minimal::rpc
