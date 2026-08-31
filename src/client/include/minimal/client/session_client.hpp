#pragma once

#include <functional>
#include <mutex>
#include <vector>
#include <atomic>
#include <memory>
#include <optional>
#include <cstdint>
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
        // "<provider>/<model>", empty = the preset's own. Outranks it.
        std::string model;
        std::string name;
        // UNSPECIFIED lets permissions.json decide. The headless client asks for
        // ALLOW_ONLY explicitly, because it has no human to answer a prompt.
        v1::PermissionRunMode run_mode = v1::PERMISSION_RUN_MODE_UNSPECIFIED;
        // Non-empty means RESUME this stored session instead of creating a new
        // one. The stored row's name/preset/project_root govern, so the three
        // fields above are not sent with it; only run_mode still applies.
        std::string resume_session_id;
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
    //
    // When `handle` is non-null it is populated BEFORE the first event is read,
    // with a handle whose Cancel() interrupts the in-flight turn (a CancelTurn
    // ClientEvent on this stream). Cancel is safe to call from another thread
    // while RunTurn's thread reads, and a no-op once the stream has closed.
    class TurnHandle;
    grpc::Status RunTurn(const std::string& session_id, const std::string& text,
                         const std::function<bool(const v1::ServerEvent&)>& on_event,
                         std::shared_ptr<TurnHandle>* handle = nullptr);

    struct CommandOptions {
        std::string name;  // without the leading '/'
        std::vector<std::string> positional;
        std::string raw;  // everything after the name, verbatim
        // Answers a ConfirmationRequest; returning "" declines. Required,
        // because a command that asks and gets no answer blocks the server's
        // worker until the stream drops -- the headless client supplies one that
        // declines, which is the same choice AllowOnly makes for permissions.
        std::function<std::string(const v1::ConfirmationRequest&)> on_confirm;
    };

    // Sends one CommandInvocation and pumps events until its CommandResult
    // arrives. Unlike RunTurn this may WRITE mid-stream: a confirming command
    // blocks server-side waiting for an answer that has to travel back up the
    // same stream.
    grpc::Status RunCommand(const std::string& session_id, const CommandOptions& options,
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
    // Shared between RunTurn's thread (which reads) and a Cancel() caller
    // (which writes). One writer at a time is the gRPC sync-stream rule, so
    // every write on the stream -- RunTurn's permission answers included --
    // takes mu. stream is null before the turn is sent and after RunTurn
    // returns, which is what makes a late Cancel a harmless no-op.
    struct TurnControl {
        std::mutex mu;
        v1::ClientEvent cancel_event;
        grpc::ClientReaderWriter<v1::ClientEvent, v1::ServerEvent>* stream = nullptr;
    };

public:
    // Cancels the turn of the RunTurn call that produced this handle. The
    // server ends the turn with a TurnCompletedEvent carrying
    // STOP_REASON_CANCELLED; RunTurn keeps reading until it arrives.
    class TurnHandle {
    private:
        // Pass-key: Token is private, so only SessionClient (a friend) can
        // name it -- the public constructor is uncallable from outside, which
        // is what std::make_shared's allocator context needs to see.
        struct Token {};

    public:
        void Cancel() {
            const auto control = control_;
            if (!control) return;
            std::lock_guard<std::mutex> lock(control->mu);
            if (control->stream != nullptr) control->stream->Write(control->cancel_event);
        }

        explicit TurnHandle(std::shared_ptr<TurnControl> control, Token)
            : control_(std::move(control)) {}

    private:
        friend class SessionClient;
        std::shared_ptr<TurnControl> control_;
    };

private:
    std::string server_address_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<v1::SessionService::Stub> stub_;
    // Monotonic command request ids; the server echoes them verbatim.
    // ATOMIC, because its own comment describes the racy case as the reason it
    // exists: "two commands on different streams of the same client would
    // echo-collide". Two concurrent RunCommand calls on one SessionClient is
    // exactly that scenario, and a plain ++ from two threads is a data race that
    // can also produce the duplicate id this is meant to prevent. The class
    // header already advertises concurrent streams, and app.cpp already runs
    // Attach on other threads against the same object.
    std::atomic<std::uint64_t> command_counter_{0};
};

}  // namespace minimal::client
