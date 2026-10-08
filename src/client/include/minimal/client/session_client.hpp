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

#include "minimal/common/logger.hpp"

#include <grpcpp/grpcpp.h>

#include "minimal/client/sdk/subscribe_outcome.hpp"
#include "minimal/v1/session_service.grpc.pb.h"
#include "minimal/client/policy.hpp"

namespace minimal::client {

// The explicit EndSession.Mode a stream ends with. The proto REQUIRES one
// (MODE_UNSPECIFIED is refused server-side); DETACH lets the session keep
// running (close/detach flow, quit teardown), TERMINATE kills the session
// and everything in flight (the close confirmation's "close anyway").
enum class EndSessionMode { kDetach, kTerminate };

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
        // Signal subscriptions ("ci/build.completed", "sessions", ...). Sent
        // either way -- like run_mode, a property of this invocation, never of
        // the stored row. Validated server-side; an unknown selector fails the
        // create.
        std::vector<std::string> signals;
        // agent_preset/model came from the project's remembered last pick,
        // not a choice made now: the server falls back instead of refusing
        // when they no longer resolve (see created() for what actually ran).
        bool remembered_selection = false;
    };

    // One persistent session stream: a single gRPC bidi with its own reader thread
    // (delivers ServerEvents to the callback) and writer thread (queues ClientEvents).
    // One Stream per UI tab. Shares this SessionClient's channel.
    class Stream {
    public:
        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;
        ~Stream();

        // Creates a fresh session -- or, when opts.resume_session_id is set,
        // RESUMES that stored session (CreateSession carries the id; the
        // stored row's name/preset/project_root govern, so the other identity
        // fields must stay empty) -- and names the bidi with a ClientEvent
        // carrying the id and an empty Attach{} payload (the documented
        // no-work naming mechanism; a payload-less event draws the server's
        // "no payload" warning), so the stream is attached even before the
        // caller's first real turn. Does NOT start reader/writer threads
        // yet. Returns the session_id, or nullopt on failure (see status()).
        std::optional<std::string> Open(const CreateOptions& opts);
        // The row a successful Open() created or resumed (what actually ran:
        // agent_preset, and model -- empty when it runs the default).
        const v1::SessionSummary& created() const { return created_; }

        // Attaches to a session ALREADY LIVE server-side by id (a subagent the
        // parent's stream just announced). Names the stream on its first
        // ClientEvent; does not start threads yet. Never fails here -- and
        // never performs a RESUME: a stored (not live) session must go through
        // Open() with resume_session_id, because OpenSession resolves its id
        // against the live registry only and would answer NOT_FOUND. The
        // returned optional is always engaged; a failed attach surfaces
        // later, as the reader's Finish() status via the on-closed callback.
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
    // any thread (idempotent via launch_state_'s kClosed latch). A bare close is read by
    // the server as a DETACH: the session keeps running (EndSession{TERMINATE} ends it).
    //
    // The bidi context is CANCELLED before the joins: a writer blocked in
    // Write() (server stopped reading) or a reader blocked in Read() (wedged
    // connection) would otherwise hang Close() -- which runs on AppState's
    // stream closer (CloseTab) or at quit -- forever. Cancel costs queued-but-unsent
    // events, which is consistent with Close's documented destructiveness:
    // the session is being torn down regardless.
    //
    // SELF-JOIN INVARIANT: ~Stream runs Close(), and ~Stream can itself run
    // ON the reader thread (the on_closed_ callback dropping the last
    // Stream reference). Close() detects that case and detaches the reader
    // instead of joining it -- joining the current thread throws, and an
    // unjoined joinable std::thread destroyed in ~Stream terminates. This
    // is safe only because the reader lambda's tail after on_closed_ is
    // empty: the thread never touches members again.
    void Close();

    // S5.4: the graceful sibling of Close(). Sends EndSession{mode} --
    // DETACH (the default: quit teardown, tab close, /detach-session; the
    // session keeps running) or TERMINATE (the close confirmation's
    // "close anyway": the session and everything in flight are destroyed)
    // -- queued ahead of closing_, so anything already pending flushes
    // first, then half-closes and joins WITHOUT the TryCancel() Close()
    // uses -- the point of an explicit EndSession is to let the writer
    // actually deliver that message rather than possibly losing it to a
    // cancel race, and the server ends this stream's RPC normally once it
    // reads the mode (Dispatch's read loop, session_service.cpp), so there
    // is nothing here for a cancel to rescue -- NORMALLY. The writer's join
    // is still bounded (2 s): a server that wedged mid-turn leaves the
    // writer blocked in a flow-control-stalled Write(), and a plain join
    // would hang the whole client at quit. Past the bound the context IS
    // cancelled: the client is quitting or destroying the tab (Detach runs
    // only from DetachAllTabs at teardown and the close-confirmation
    // paths), so losing the unsent tail and ending the RPC with a
    // CANCELLED status is acceptable there. With DETACH the SESSION keeps
    // running; only this stream ends. Safe from any thread, idempotent
    // (shares Close()'s launch_state_ kClosed latch -- whichever of the two
    // runs first wins). A stream whose Resume() never ran before this fires
    // is reaped directly (cancel + Finish()), not via the graceful path
    // below -- see the definition.
    void Detach(EndSessionMode mode = EndSessionMode::kDetach);

        // Unblocks the underlying call without the full Close() discipline
        // (joins, Finish). For streams being discarded without ever having
        // been resumed -- a duplicate AttachTab that must not terminate the
        // session the way closing its second bidi would.
        void Cancel();

        // Thread-safe snapshot of the RPC outcome: Finish()'s status after
        // Close(), or the reader's error when the server died mid-stream.
        grpc::Status status() const;
        const std::string& session_id() const { return session_id_; }
        bool resumed() const {
            std::lock_guard<std::mutex> lk(launch_mtx_);
            return ever_resumed_;
        }

        // True once the reader delivered at least one ServerEvent. The
        // on-closed handler's one reliable attach-refusal discriminator: a
        // stream the server refused (unknown/terminated session at OpenSession)
        // ends WITHOUT ever having delivered an event, while a stream that was
        // genuinely attached and later terminated ends with the same NOT_FOUND
        // status after a lifetime of events. Status code alone cannot tell
        // these apart; this can.
        bool received_any() const { return received_any_.load(std::memory_order_acquire); }

    private:
        friend class SessionClient;
        Stream(std::shared_ptr<grpc::Channel> ch, EventCallback cb);

    void LaunchReaderThread();
    void LaunchWriterThread();
    void OpenBidi();             // constructs the gRPC bidi once (lazy); threads start in Resume()
    void EnqueueNamingEvent();   // queue the naming ClientEvent (session_id + empty Attach{})
    // Joins the reader thread, EXCEPT on the reader thread itself (see
    // Close()'s self-join invariant): there it detaches instead -- joining
    // the current thread throws and would std::terminate via ~Stream.
    void JoinReaderOrDetachSelf();
    // Waits (bounded by kDetachJoinTimeout) for `exited_flag` to latch under
    // qmtx_; past the bound, cancels the call and waits again, unbounded --
    // the cancel guarantees the blocked Write()/Read() unblocks promptly.
    // Shared by Detach()'s writer_/reader_ joins (identical logic, different
    // thread/cv/flag).
    void WaitForExit(std::condition_variable& cv, bool& exited_flag);

        std::shared_ptr<grpc::Channel> channel_;
        std::unique_ptr<v1::SessionService::Stub> stub_;

        EventCallback event_cb_;
        std::function<void(const grpc::Status&)> on_closed_;
        std::string session_id_;
        v1::SessionSummary created_;

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
        std::atomic<bool> received_any_{false};   // reader: first event delivered (see received_any())

        // Bounded (kStreamSendQueueCap) like the server's outbound queue: a
        // stream whose server side stopped reading must not grow client memory
        // without limit.

        // Guards status_: written by Open/Attach (constructor thread), the
        // reader thread (its own Finish), and Close(); read via status() from
        // any thread.
        mutable std::mutex status_mtx_;
        grpc::Status status_;
        void SetStatus(const grpc::Status& status);

        std::thread writer_;
        std::thread reader_;
        // Bounded-join support for Detach(): a plain join() has no timeout,
        // and a writer blocked in a flow-control-stalled Write() (wedged
        // server) would hang quit forever. The writer latches writer_exited_
        // under qmtx_ just before returning; Detach waits with a timeout on
        // writer_cv_, cancels past the bound, waits again.
        std::condition_variable writer_cv_;
        bool writer_exited_ = false;   // guarded by qmtx_; set once, near the writer's end
        // Same bounded-join support, mirrored for the reader: the writer's
        // bound alone isn't enough, because WritesDone() needs no reply and
        // so finishes quickly whether or not the SERVER is still alive --
        // Detach() then fell to a plain, unbounded reader join, and a server
        // that stopped responding but never closed the socket hung quit
        // forever. Latched under qmtx_ at the same point closing_ already
        // is (right after the reader's own Finish(), before on_closed_).
        std::condition_variable reader_cv_;
        bool reader_exited_ = false;   // guarded by qmtx_; set once, near the reader's end

        // Single source of truth for the resume/close race. Resume() (does it
        // get to launch threads?) and Close()/Detach() (were there ever any
        // threads to join?) used to answer from two independent atomics
        // (resumed_, closed_) with no shared critical section -- a Detach()
        // landing between the owning AppState registering this stream's tab
        // and its own Resume() call could latch "closed" while Resume() had
        // already read "not resumed" and was about to launch reader_/writer_
        // anyway (a joinable thread pair destroyed moments later in ~Stream
        // -> std::terminate); or Detach() could find no writer thread to
        // bound its wait against and, since its graceful path deliberately
        // skips an up-front cancel, never issue one at all -- Finish() then
        // blocks forever waiting for a naming event that was queued but
        // never sent (a hang at quit). One mutex-guarded transition makes
        // "may I start" and "how do I tear down" the same decision: Resume()
        // launches the threads from INSIDE the critical section (the thread
        // constructors return immediately, so this cannot deadlock), and
        // Close()/Detach() read whether that already happened before
        // deciding how to reap the call.
        enum class LaunchState { kIdle, kRunning, kClosed };
        mutable std::mutex launch_mtx_;
        LaunchState launch_state_ = LaunchState::kIdle;
        bool ever_resumed_ = false;   // guarded by launch_mtx_; latched once Resume() actually ran
    };

    explicit SessionClient(std::string server_address);
    ~SessionClient();

    // Cancels every live PluginSubscribe reader (the destructor's sweep,
    // callable early). The TUI calls this BEFORE its plugin environment is
    // destroyed: plugin destructors run at env teardown and a subscribing
    // plugin that joins its worker there would otherwise deadlock -- the
    // worker parked in Read() on a stream nobody has cancelled yet, while
    // ~SessionClient's own sweep only fires later (after env is already
    // gone). Idempotent; safe from any thread.
    void CancelSubscriptions();

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

    // Presence heartbeat (see PingRequest in the proto). Status is rarely
    // interesting: the TUI's heartbeat thread calls this on a loop and
    // ignores failures -- a dead server makes every beat fail until the
    // process exits, which is exactly the signal the server's reaper uses
    // in reverse.
    grpc::Status Ping();

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
    // Any page shape (after_seq delta pages included), with the same bounded
    // deadline -- what the TUI's scrollback fetch and the plugin seam use.
    GetMessagesResult GetMessagesPage(const v1::GetMessagesRequest& request);

    // The session picker's list. project_root empty requires include_all_dirs.
    struct ListSessionsOptions {
        std::string project_root;
        bool include_all_dirs = false;
        // Subagent rows (parent_session_id set) are hidden unless set -- the
        // startup group-attach scan is the caller that needs them.
        bool include_subagents = false;
        int32_t limit = kSessionListPageSize;
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

    // The request id a /-command invocation carries ("cmd-N"), echoed on its
    // CommandResult. ONE counter per client, for the headless and the TUI
    // paths alike: two commands on two streams must never echo-collide.
    std::string NextCommandRequestId() {
        return "cmd-" + std::to_string(command_counter_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    // Every page of a listing, following the (updated_at, id) keyset cursor
    // until a short page ends it. `options`' cursor fields are ignored (the
    // walk starts at the first page); `options.limit` is the page size.
    // `keep` filters rows as they arrive (null keeps all). On a failed page
    // the rows gathered so far come back with that page's status.
    struct ListAllSessionsResult {
        std::vector<v1::SessionSummary> sessions;
        grpc::Status status;
    };
    ListAllSessionsResult ListAllSessions(
        ListSessionsOptions options,
        const std::function<bool(const v1::SessionSummary&)>& keep = nullptr);

    // One stored row by id (the startup --resume probe). NOT_FOUND for an
    // unknown id; a subagent row answers normally with parent_session_id set.
    // is_live comes from the live registry, like ListSessions'.
    struct GetSessionResult {
        v1::SessionSummary summary;
        grpc::Status status;
    };
    GetSessionResult GetSession(const std::string& session_id);

    // A new top-level session forked from `source_session_id`'s current state
    // (ForkSessionRequest: cut at the clean boundary, source untouched). A
    // non-empty instruction becomes the fork's first user turn. The returned
    // session is LIVE server-side -- attach it, don't resume it.
    struct ForkSessionResult {
        v1::SessionSummary summary;
        grpc::Status status;
    };
    ForkSessionResult ForkSession(const std::string& source_session_id,
                                  const std::string& instruction);

    // The answering server's build identity (see ServerInfo in the proto).
    // UNIMPLEMENTED means a server older than the RPC -- see CheckServerBuild.
    struct ServerInfoResult {
        v1::ServerInfo info;
        grpc::Status status;
    };
    ServerInfoResult GetServerInfo();

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

    // The same suggestions for a SESSIONLESS tab (no session row yet): the
    // request scopes itself to `project_root` directly, and the answer is
    // relative to that root. An unknown/non-directory root is NOT_FOUND.
    SuggestPathsResult SuggestPathsForRoot(const std::string& project_root,
                                           const std::string& prefix, int32_t limit);

    // The /-command catalog: the GLOBAL half (plugins load at startup and
    // config is read once, so it cannot change mid-connection) plus, when
    // `project_root` is given, that project's own commands-md discoveries
    // (ProjectCommandSource, resolved fresh per call server-side -- a project
    // command may shadow a global one of the same name). Feeds the prompt
    // editor's /-autocomplete and the /help overlay.
    struct ListCommandsResult {
        v1::ListCommandsResponse response;
        grpc::Status status;
    };
    // `session_id`, when set, scopes the project half to that session's own
    // root (server-resolved) instead of `project_root`.
    ListCommandsResult ListCommands(const std::string& project_root = {},
                                    const std::string& session_id = {});

    // Executes a SESSIONLESS /-command (a Sessionless<> command on the
    // server: /help, /sessions and their future siblings). Dispatched with
    // no session at all -- the wire path that lets a sessionless tab run a
    // command without minting a row. Bounded like ListCommands; a transport
    // failure arrives as an ERROR CommandResult (never a dropped answer).
    v1::CommandResult RunSessionlessCommand(const std::string& name,
                                            const std::vector<std::string>& positional,
                                            const std::string& raw);

    // The agent picker's list: every agents/*.md preset's name + description,
    // fetched fresh on each open (unlike ListCommands, config can gain a new
    // preset file between opens and there is no invalidation machinery to
    // notice that, so this simply asks every time -- the list is small).
    // `project_root` scopes the listing to the client's project (project-local
    // agents/ merged, project shadows global); empty means global only.
    struct ListAgentPresetsResult {
        v1::ListAgentPresetsResponse response;
        grpc::Status status;
    };
    ListAgentPresetsResult ListAgentPresets(const std::string& project_root = {});

    // The model picker's list: every configured or discovered model ref.
    // refresh_discovery re-fetches every discover-entry's catalog first
    // (provider catalogs are fetched in parallel server-side, but the wall
    // clock is still bounded by the slowest provider -- hence the longer
    // deadline than a pure listing gets). The bare listing never triggers a
    // fetch, so it keeps the short standard deadline.
    struct ListModelsOptions {
        bool refresh_discovery = false;
    };
    struct ListModelsResult {
        v1::ListModelsResponse response;
        grpc::Status status;
    };
    ListModelsResult ListModels(const ListModelsOptions& options);

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
    // Per-turn agent/model overrides ("the turn borrows, the session keeps",
    // server-side): resolved at turn pop, a failed resolve fails the TURN
    // with a named error and leaves the session's own state untouched. What
    // the headless client uses to run one turn on a preset/model that is not
    // the session's -- notably --resume, where the stored row's preset
    // governs but --agent must still reach the turn.
    struct TurnOverrides {
        std::string model;          // "<provider>/<model>"
        std::string agent_preset;   // preset name
    };
    grpc::Status RunTurn(const std::string& session_id, const std::string& text,
                         const std::function<bool(const ServerEvent&)>& on_event,
                         const std::function<void(const std::shared_ptr<TurnHandle>&)>&
                             on_handle = nullptr,
                         const TurnOverrides& overrides = {});

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
    //
    // cancel_pending closes the publication race: the handle is published
    // (on_handle) BEFORE RunTurn stores `stream`, so a Cancel arriving in
    // that window used to find stream == nullptr and silently no-op -- the
    // interrupt was lost. It latches here instead, and RunTurn delivers the
    // cancel right after storing the stream, before its first Read().
    struct TurnControl {
        std::mutex mu;
        v1::ClientEvent cancel_event;
        grpc::ClientReaderWriter<v1::ClientEvent, v1::ServerEvent>* stream = nullptr;
        bool cancel_pending = false;
    };

public:
    // Cancels the turn of the RunTurn call that produced this handle. The
    // server ends the turn with a TurnCompletedEvent carrying
    // STOP_REASON_CANCELLED; RunTurn keeps reading until it arrives. Safe to
    // call before the turn's stream exists (the cancel is remembered and
    // delivered by RunTurn the moment it stores the stream) and a no-op once
    // the stream has closed.
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
            if (control->stream != nullptr) {
                // A failed Write means the interrupt never left -- gRPC sync
                // Write returns false when the stream is dead/cancelled. The
                // reader's Finish will surface the transport death, but say
                // so here instead of dropping the press silently.
                if (!control->stream->Write(control->cancel_event)) {
                    common::DefaultLogger().warn("turn cancel could not be written "
                                                 "(stream already dead)");
                }
            } else {
                // Published before RunTurn stored the stream (the on_handle
                // window): remember it -- RunTurn delivers the cancel right
                // after storing -- rather than silently dropping the
                // interrupt.
                control->cancel_pending = true;
            }
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

    // Teardown registry for live PluginSubscribe calls. SHARED with each
    // subscriber (shared_ptr): ~SessionClient's cancel sweep and the
    // subscriber's own erase (PluginSubscribe's exit paths) both touch this
    // set, and a woken subscriber can run AFTER ~SessionClient's body --
    // with the set as a plain member its erase raced the member's own
    // destruction (use-after-destroy). The registry dies with the LAST
    // holder instead, so the erase is always on a live object.
    struct SubscribeRegistry {
        std::mutex mtx;
        std::unordered_set<grpc::ClientContext*> active;
        // Latched by CancelSubscriptions (guarded by mtx, same as active):
        // without this, CancelSubscriptions was a one-time SWEEP, not a
        // latch -- a plugin that re-subscribes AFTER it runs (its own
        // retry-on-error logic reacting to the kError/kCancelled the sweep
        // just produced) got a fresh, uncancelled context with no deadline.
        // Nothing would ever cancel it, and that plugin's own destructor
        // joining its subscriber thread hung quit -- the exact deadlock
        // CancelSubscriptions exists to prevent, reappearing one level down.
        bool shutting_down = false;
    };
    std::shared_ptr<SubscribeRegistry> subscribes_ = std::make_shared<SubscribeRegistry>();

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
