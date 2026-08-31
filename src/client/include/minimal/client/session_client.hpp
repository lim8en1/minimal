#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "minimal/v1/session_service.grpc.pb.h"

namespace minimal::client {

// Transport layer for the TUI: owns one gRPC channel and spawns persistent per-tab
// streams. The headless client (`-p`) still uses RunTurn() directly below; the two
// share a channel but are independent RPC lifecycles.
class SessionClient {
public:
    using ServerEvent = v1::ServerEvent;
    using EventCallback = std::function<void(const ServerEvent&)>;

    // Options for creating a new session/stream (also drives CreateSession). Declared here,
    // above the nested Stream class, so Open()/Attach() can reference it by name.
    struct CreateOptions {
        std::string project_root;
        std::string agent_preset;
        std::string name;
        v1::PermissionRunMode run_mode = v1::PERMISSION_RUN_MODE_UNSPECIFIED;
    };

    // One persistent session stream: a single gRPC bidi with its own reader thread
    // (delivers ServerEvents to the callback) and writer thread (queues ClientEvents).
    // One Stream per UI tab. Shares this SessionClient's channel.
    class Stream {
    public:
        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;
        ~Stream();

        // Creates a fresh session (calls CreateSession) and names the bidi with an
        // empty ClientEvent carrying the id — so the stream is attached even before the
        // caller's first real turn. Does NOT start reader/writer threads yet. Returns the
        // new session_id, or nullopt on failure (see status()).
        std::optional<std::string> Open(const CreateOptions& opts);

        // Attaches to an already-existing server-side session by id (resume / subagent).
        // Names the stream on its first ClientEvent; does not start threads yet. Returns
        // the session_id, or nullopt if OpenSession failed.
        std::optional<std::string> Attach(const std::string& session_id);

        // Starts the reader + writer threads. Call AFTER the owning tab is registered in
        // AppState so a dispatched event can never find an unregistered entry. Safe to
        // call once; idempotent.
        void Resume();

        // Fires exactly once, from the reader thread, when the server closes this
        // stream (session terminated) -- as opposed to a normal turn completing,
        // which the stream stays open across. Set BEFORE Resume(), same two-phase
        // discipline as the event callback: the owning tab must be registered
        // first so this can never be dispatched to something that doesn't exist.
        void SetOnClosed(std::function<void()> cb);

        // Thread-safe enqueue of any ClientEvent (turn input, command invocation,
        // permission/confirmation answer, cancel). Non-blocking: queued and drained by the
        // writer thread. Dropped silently if closing/closed.
        void Send(const v1::ClientEvent& event);

        // Half-close our send side, drain remaining server output, then Finish(). Safe from
        // any thread (idempotent via closed_ latch). Closing the last stream of a session
        // terminates it server-side; closing one tab leaves others running.
        void Close();

        grpc::Status status() const { return status_; }
        const std::string& session_id() const { return session_id_; }
        bool resumed() const { return resumed_.load(); }

    private:
        friend class SessionClient;
        Stream(std::shared_ptr<grpc::Channel> ch, EventCallback cb);

        void LaunchReaderThread();
        void LaunchWriterThread();
        grpc::Status OpenBidi();   // opens the gRPC bidi once; threads start in Resume()
        void EnqueueNamingEvent();   // queue an empty ClientEvent that names the stream

        std::shared_ptr<grpc::Channel> channel_;
        std::unique_ptr<v1::SessionService::Stub> stub_;

        EventCallback event_cb_;
        std::function<void()> on_closed_;
        std::string session_id_;
        grpc::Status status_;

        // Must outlive the bidi below -- gRPC requires the ClientContext used to
        // create a call to stay alive for the call's whole lifetime, not just the
        // OpenBidi() call that creates it.
        grpc::ClientContext context_;

        // gRPC bidi for this session. The concrete writer type is grpc::ClientReaderWriter,
        // NOT a Stub nested type — OpenSession() returns exactly this.
        std::unique_ptr<grpc::ClientReaderWriter<v1::ClientEvent, v1::ServerEvent>> bidi_;
        std::mutex qmtx_;
        std::condition_variable send_cv_;
        std::deque<v1::ClientEvent> send_queue_;
        std::atomic<bool> closing_{false};

        std::thread writer_;
        std::thread reader_;
        std::atomic<bool> resumed_{false};   // threads started at most once
        std::atomic<bool> closed_{false};    // Close() called
    };

    explicit SessionClient(std::string server_address);
    ~SessionClient();

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    // Waits up to `timeout` for the shared channel to be usable. Reported rather than
    // thrown: "no server running" is ordinary for a client expected to spawn one.
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

    // Creates a new persistent Stream sharing this channel. The callback is the one place
    // every tab's reader funnels events: AppState.DispatchToTab keys off each event's own
    // session_id, so all streams can share a single dispatcher.
    std::shared_ptr<Stream> MakeStream(EventCallback cb);

    // --- Headless single-turn plumbing (kept intact; see app.cpp) ---

    // Returns the new session id, or a failure status.
    struct CreateResult {
        std::string session_id;
        grpc::Status status;
    };
    CreateResult CreateSession(const CreateOptions& options);

    // Scrollback, keyset-paginated -- a separate RPC from the live bidi stream so
    // paging through a 512k-1M token history never competes with turn traffic.
    // before_seq is the exclusive upper bound (nullopt = most recent page); see
    // GetMessagesRequest for why this is optional rather than a 0-means-newest
    // sentinel. Feeds directly into SessionState::ApplyScrollbackPage.
    struct GetMessagesResult {
        v1::GetMessagesResponse response;
        grpc::Status status;
    };
    GetMessagesResult GetMessages(const std::string& session_id,
                                  std::optional<int64_t> before_seq, int32_t limit);

    // Opens a bidi stream for one turn: sends UserChatTurn, pumps ServerEvents to
    // `on_event` until TurnCompletedEvent or stream end. Reads TO END before Finish()
    // (the read loop only returns once the server stops sending), so no events are lost
    // and Finish() cannot deadlock a still-writing peer.
    grpc::Status RunTurn(const std::string& session_id, const std::string& text,
                         const std::function<bool(const ServerEvent&)>& on_event);

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
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<v1::SessionService::Stub> stub_;
    // Monotonic command request ids; the server echoes them verbatim.
    std::uint64_t command_counter_ = 0;
};

}  // namespace minimal::client
