#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <grpcpp/grpcpp.h>

#include "minimal/v1/session_service.grpc.pb.h"

namespace minimal::client {

// Owns one gRPC stream per session and demultiplexes ServerEvents into the right
// per-tab state by session_id. One client holds several of these: the focused
// session, plus any subagent children.
//
// Attaching to a child session on SubagentStartedEvent is a PROTOCOL OBLIGATION,
// not a TUI behavior -- a never-attached session is terminated by the server's
// initial-attach guard. The headless -p client must attach too.
class SessionClient {
public:
    explicit SessionClient(std::string server_address);
    ~SessionClient();

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    // Waits up to `timeout` for the channel to be usable. Reported rather than
    // thrown: "no server is running" is the ordinary case for a client that is
    // expected to spawn one, not an exceptional condition.
    bool WaitForConnected(std::chrono::milliseconds timeout);

    struct CreateOptions {
        std::string project_root;
        std::string agent_preset;
        std::string name;
        // UNSPECIFIED lets permissions.json decide. The headless client asks for
        // ALLOW_ONLY explicitly, because it has no human to answer a prompt.
        v1::PermissionRunMode run_mode = v1::PERMISSION_RUN_MODE_UNSPECIFIED;
    };

    // Returns the new session id, or the failure status.
    struct CreateResult {
        std::string session_id;
        grpc::Status status;
    };
    CreateResult CreateSession(const CreateOptions& options);

    // Opens the bidi stream, sends one user turn, and pumps ServerEvents to
    // `on_event` until the stream ends or `on_event` returns false.
    //
    // The stream is READ TO END before finishing. gRPC's sync API blocks in
    // Write() when the peer is not reading, so finishing early with events still
    // in flight deadlocks both sides.
    grpc::Status RunTurn(const std::string& session_id, const std::string& text,
                         const std::function<bool(const v1::ServerEvent&)>& on_event);

    // Opens the stream and ATTACHES without submitting a turn -- what a client
    // owes a subagent child announced by SubagentStartedEvent. Attaching lifts
    // the initial-attach guard; the session is otherwise untouched, so its
    // events simply flow to `on_event`. Same read-to-end discipline as RunTurn:
    // returning false from `on_event` half-closes and drains; the server
    // terminates the session when the stream closes.
    grpc::Status Attach(const std::string& session_id,
                        const std::function<bool(const v1::ServerEvent&)>& on_event);

private:
    std::string server_address_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<v1::SessionService::Stub> stub_;
};

}  // namespace minimal::client
