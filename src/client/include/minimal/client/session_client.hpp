#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "minimal/client/subscribe_outcome.hpp"
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
        //
        // The reader's Finish() status is handed TO the callback: the callback
        // must never reach back into this Stream (a GetStream copy made here can
        // be the last owner while ~Stream is running, and the reader would then
        // join itself -- std::terminate).
        void SetOnClosed(std::function<void(const grpc::Status&)> cb);

        // Thread-safe enqueue of any ClientEvent (turn input, command invocation,
        // permission/confirmation answer, cancel). Non-blocking: queued and drained by the
        // writer thread. Returns FALSE when the event was dropped: the stream is
        // closing/closed, or the bounded send queue is full (the server stopped
        // reading) -- the caller should surface that rather than retry blindly.
        bool Send(const v1::ClientEvent& event);

        // Half-close our send side, drain remaining server output, then Finish(). Safe from
        // any thread (idempotent via closed_ latch). Closing the last stream of a session
        // terminates it server-side; closing one tab leaves others running.
        //
        // The bidi context is CANCELLED before the joins: a writer blocked in
        // Write() (server stopped reading) or a reader blocked in Read() (wedged
        // connection) would otherwise hang Close() -- which runs on the UI
        // thread from CloseTab/quit -- forever. Cancel costs queued-but-unsent
        // events, which is consistent with Close's documented destructiveness:
        // the session is being torn down regardless.
        void Close();

        // Unblocks the underlying call without the full Close() discipline
        // (joins, Finish). For streams being discarded without ever having
        // been resumed -- a duplicate AttachTab that must not terminate the
        // session the way closing its second bidi would.
        void Cancel();

        // Thread-safe snapshot of the RPC outcome: Finish()'s status after
        // Close(), or the reader's error when the server died mid-stream.
        grpc::Status status() const;
        const std::string& session_id() const { return session_id_; }
        bool resumed() const { return resumed_.load(); }

    private:
        friend class SessionClient;
        Stream(std::shared_ptr<grpc::Channel> ch, EventCallback cb);

    void LaunchReaderThread();
    void LaunchWriterThread();
    void OpenBidi();             // constructs the gRPC bidi once (lazy); threads start in Resume()
    void EnqueueNamingEvent();   // queue an empty ClientEvent that names the stream

        std::shared_ptr<grpc::Channel> channel_;
        std::unique_ptr<v1::SessionService::Stub> stub_;

        EventCallback event_cb_;
        std::function<void(const grpc::Status&)> on_closed_;
        std::string session_id_;

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

        // Bounded like the server's outbound EventQueue (65k) -- a stream whose
        // server side stopped reading must not grow client memory without
        // limit. 256 events ≈ an entire turn's control traffic several times
        // over; beyond that the stream is stalled and Send() refuses.
        static constexpr std::size_t kSendQueueCap = 256;

        // Guards status_: written by Open/Attach (constructor thread), the
        // reader thread (its own Finish), and Close(); read via status() from
        // any thread.
        mutable std::mutex status_mtx_;
        grpc::Status status_;
        void SetStatus(const grpc::Status& status);

        std::thread writer_;
        std::thread reader_;
        std::atomic<bool> resumed_{false};   // threads started at most once
        std::atomic<bool> closed_{false};    // Close() called
    };

    explicit SessionClient(std::string server_address);
    ~SessionClient();

    // Cancels live PluginSubscribe calls at teardown: a subscriber still
    // blocked in Read() when the client dies would otherwise race destruction.
    std::mutex subscribe_mtx_;
    std::unordered_set<grpc::ClientContext*> active_subscribes_;

    SessionClient(const SessionClient&) = delete;
    SessionClient& operator=(const SessionClient&) = delete;

    // Waits up to `timeout` for the shared channel to be usable. Reported rather than
    // thrown: "no server running" is ordinary for a client expected to spawn one.
    bool WaitForConnected(std::chrono::milliseconds timeout);

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

    // Caller-owned-context variant (the TUI bounds its fetches with a deadline).
    grpc::Status GetMessages(grpc::ClientContext* context, const v1::GetMessagesRequest& request,
                             v1::GetMessagesResponse* response);

    // The session picker's list. project_root empty requires include_all_dirs.
    struct ListSessionsOptions {
        std::string project_root;
        bool include_all_dirs = false;
        int32_t limit = 100;
        // Keyset cursor (exclusive upper bound on updated_at, tie-broken by
        // id), mirroring ListSessionsRequest's before_updated_at/before_id --
        // both empty/nullopt means "first page". The picker's "load more"
        // (T8) is the first caller; before that, this was on the wire and
        // reachable from no client call site at all.
        std::optional<int64_t> before_updated_at;
        std::optional<std::string> before_id;
    };
    struct ListSessionsResult {
        v1::ListSessionsResponse response;
        grpc::Status status;
    };
    ListSessionsResult ListSessions(const ListSessionsOptions& options);

    // @-mention suggestions, resolved by the server's MentionResolver so the
    // strategy is pluggable server-side and every frontend inherits it. Paths
    // are relative to the session's project_root; directories carry a trailing
    // '/'. An unknown session is NOT_FOUND.
    struct SuggestPathsResult {
        v1::SuggestPathsResponse response;
        grpc::Status status;
    };
    SuggestPathsResult SuggestPaths(const std::string& session_id, const std::string& prefix,
                                    int32_t limit);

    // The /-command catalog, fetched ONCE at connect (plugins load at startup
    // and config is read once, so the list cannot change mid-connection).
    // Feeds the prompt editor's /-autocomplete.
    struct ListCommandsResult {
        v1::ListCommandsResponse response;
        grpc::Status status;
    };
    ListCommandsResult ListCommands();

    // Deletes a stored session that has no stream open. The picker's delete
    // action closes the tab first; the server refuses an open session.
    grpc::Status DeleteSession(const std::string& session_id);

    // --- The generic plugin channel (E5; docs/EXTENSIBILITY.md 2.5) ----------
    //
    // A client plugin's OWN data path: the payloads are plugin-owned JSON that
    // core neither defines nor interprets. The plugin names its server-side
    // pair; the server dispatches through the ServiceRegistry.

    // One call, one JSON answer. status not-ok covers unknown plugin / refused
    // method / endpoint throw -- the message names which.
    struct PluginCallResult {
        std::string json_payload;
        grpc::Status status;
    };
    PluginCallResult PluginCall(const std::string& plugin_name, const std::string& method,
                                const std::string& json_payload);

    SubscribeOutcome PluginSubscribe(const std::string& plugin_name, const std::string& method,
                                     const std::string& json_payload,
                                     const std::function<bool(const std::string& json_payload)>& on_payload,
                                     grpc::Status* status = nullptr);

    // Opens the bidi stream, sends one user turn, and pumps ServerEvents to
    // `on_event` until the stream ends or `on_event` returns false.
    //
    // The stream is READ TO END before finishing. gRPC's sync API blocks in
    // Write() when the peer is not reading, so finishing early with events still
    // in flight deadlocks both sides.
    //
    // `on_handle` is invoked ON RUNTURN'S THREAD, once, as soon as the handle
    // exists and BEFORE the first event is read. The handle's Cancel()
    // interrupts the in-flight turn (a CancelTurn ClientEvent on this stream);
    // Cancel is safe to call from another thread while RunTurn's thread reads,
    // and a no-op once the stream has closed. Publication is a callback rather
    // than an out-param because the reader is BY DESIGN another thread (SIGINT
    // watcher): a plain cross-thread read of a shared_ptr this thread wrote is
    // a data race, while a callback lets the caller store the handle behind a
    // release-store flag the cancel thread acquires.
    class TurnHandle;
    grpc::Status RunTurn(const std::string& session_id, const std::string& text,
                         const std::function<bool(const ServerEvent&)>& on_event,
                         const std::function<void(const std::shared_ptr<TurnHandle>&)>&
                             on_handle = nullptr);

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
