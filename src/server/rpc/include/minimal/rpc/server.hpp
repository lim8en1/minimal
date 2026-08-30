#pragma once

// Builds and runs the gRPC server on a unix socket.
//
// A unix socket, not a TCP port: the topology is local and single-user, so
// filesystem permissions are the access control and there is no port to collide
// with or accidentally expose. See ARCHITECTURE.md.
//
// NO DAEMON. The server is refcounted by attached clients and shuts down when
// the last one leaves. Auto-spawning from the client is deferred, but this is
// the half it will depend on -- a client that spawns a server must be able to
// rely on it going away again.

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "minimal/rpc/session_service.hpp"

namespace minimal::rpc {

// The default endpoint: <data dir>/server.sock, as a gRPC "unix://" target.
std::filesystem::path DefaultSocketPath();
std::string SocketTarget(const std::filesystem::path& socket_path);

class Server {
public:
    // `socket_path` is a filesystem path, not a gRPC target string; the "unix://"
    // prefix is this class's business.
    Server(SessionServiceImpl& service, std::filesystem::path socket_path);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Null if the socket could not be bound. Reported rather than thrown so the
    // caller can print a usable diagnostic (usually: another server is already
    // running on this socket).
    bool listening() const { return server_ != nullptr; }

    const std::filesystem::path& socket_path() const { return socket_path_; }
    std::string target() const { return SocketTarget(socket_path_); }

    // Blocks until shutdown is requested and the server has drained.
    void Wait();

    // ASYNCHRONOUS, and that is the point. grpc::Server::Shutdown() blocks until
    // every in-flight RPC completes, so calling it FROM an RPC handler deadlocks:
    // the handler waits for itself. The last-client-detached callback fires at
    // the tail of OpenSession, which is exactly that situation -- so it must be
    // able to ask for shutdown without performing it. This only sets a flag; a
    // separate thread inside Wait() does the blocking part.
    void RequestShutdown();

    // Synchronous. Safe only from a thread that is not serving an RPC.
    void Shutdown();

private:
    std::filesystem::path socket_path_;
    std::unique_ptr<grpc::Server> server_;
    std::mutex mutex_;
    std::condition_variable shutdown_cv_;
    bool shutdown_requested_ = false;
};

}  // namespace minimal::rpc
