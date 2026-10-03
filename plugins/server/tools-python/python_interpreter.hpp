#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/tools/cancel_token.hpp"
#include "minimal/tools/tool.hpp"

#include "python_process.hpp"
#include "python_settings.hpp"
#include "stream_ring.hpp"

namespace minimal::tools_python {

// Everything needed to spawn one interpreter. `declared` is the preset's
// DECLARED package contract at start time (packages + requirements: paths) --
// what later per-exec diffs compare against. `session_lifetime` is the
// interpreter's clock: the reader polls it and kills the process on expiry
// (session terminate is the one non-explicit kill path).
struct InterpreterConfig {
    std::string python_path;             // interpreter binary (resolved on PATH at launch)
    std::filesystem::path cwd;           // project root (the child's working dir)
    std::filesystem::path venv;          // empty = system python (bare)
    std::vector<std::string> declared;
    std::map<std::string, std::string> env;  // the child's full environment
    std::weak_ptr<const void> session_lifetime;
};

struct RequestOutcome {
    bool ok = false;
    std::string result;       // repr of the trailing expression ('' for statements)
    std::string stdout_text;  // captured, already capped driver-side
    std::string stderr_text;  // tracebacks land here
    // What the request's code wrote to the process-level fd 1/2 (subprocess
    // output, os.system, C extensions) while it ran -- attributed by the
    // parent from the diagnostics pipe, best-effort, capped like stdout.
    std::string process_output;
    std::string error;        // transport-level failure description when !ok
    // The exception class + message on a failed request.
    std::string exc_type;
    std::string exc_msg;
    // Set when the request's max_runtime fired mid-run: the SIGINT did not
    // prevent completion, so the result is real but the deadline was felt.
    bool deadline_interrupted = false;
};

// The lifetime-safe handles a request needs to deliver a completion that
// outlives its tool call (and to learn about session death). Captured from
// the ToolContext per invocation; all of them are safe to hold past the
// session's death -- schedule_injection refuses, emit_task_event pushes into a
// closed queue, session_lifetime expires (tools/tool.hpp).
struct RequestHandles {
    std::string tool_use_id;
    std::string label;  // short code snippet for logs and panel rows
    std::function<bool(llm::Message)> schedule_injection;
    std::function<void(const tools::BackgroundTaskEvent&)> emit_task_event;
    // Incremental stdout of the RUNNING request (lossy, tail-only downstream --
    // the operator's panel; the authoritative record is the response capture).
    std::function<void(std::string_view)> emit_output;
    std::weak_ptr<const void> session_lifetime;
};

// One row of `python_output`'s task listing (no output text; call
// ReadTaskOutput for that). `seconds` is the running request's age since its
// run started, or a finished request's total runtime.
struct TaskStatus {
    enum class State { kRunning, kQueued, kFinished };
    std::string id;
    State state = State::kQueued;
    bool ok = false;                 // finished only
    double seconds = 0;
    std::uint64_t queued_behind = 0;  // queued only: requests ahead of it
    std::int64_t max_runtime = 0;     // armed deadline in seconds (0 = none)
    std::uint64_t stream_total = 0;   // running only: logical stream size so far
};

// One read of a task's output. Running tasks read from the live ring
// (absolute offsets over the logical stream, gap markers inline); finished
// tasks read from the authoritative capped capture (offsets over it; `lost`
// is always 0 there -- its own truncation marker rides inside the text).
struct OutputRead {
    bool found = false;
    bool finished = false;
    StreamRing::ReadResult slice;
};

// One persistent interpreter: the process, its reader thread, and the request
// queue. THE lifetime contract (deliberate deviation from background bash's
// background_max_timeout_ms): a bash task is a stateless process, so a timer
// may kill it; an interpreter is SHARED SESSION STATE, so no SYNCHRONOUS
// wait ever destroys it -- timeouts demote a sync wait to async. The ONE
// destructive timer is the request's own opt-in max_runtime: on expiry the
// reader soft-interrupts and, if the request ignores SIGINT for
// grace_seconds, hard-kills the interpreter (the caller's explicit deadline).
// Otherwise the interpreter's clock is the session's clock: killed when the
// session terminates (reader polls session_lifetime), when the server exits
// (the atexit pgid sweep), or by an EXPLICIT kill (python_interrupt(kill=true)
// / clean=true / /python restart).
//
// The reader thread is the liveness source of truth: EOF, protocol garbage,
// or an external kill all converge on HardKill -- the process is killed and
// reaped (never left running unread), pending async requests get injected
// error results, pending sync waits are woken with a dead outcome, and the
// next python_exec auto-restarts.
//
// THREAD/OWNERSHIP CONTRACT: the reader runs JOINABLE and is joined in the
// destructor after a HardKill (EOF and the wake pipe end its poll). It also
// owns every write to the driver's request pipe (Submit only queues). It holds
// NO shared_ptr to the interpreter -- destruction is what kills it -- so
// dropping the last reference (the store erasing the session, the fixture
// dying) reliably kills the process and reaps the thread. A detached
// self-owning reader would leak one python process per session that ended
// without an explicit kill.
class Interpreter {
public:
    // Spawns the process and waits for the ready handshake (bounded by
    // settings.startup_timeout). Null with *error set on any failure; the
    // reader thread exists only on success.
    static std::shared_ptr<Interpreter> Spawn(const InterpreterConfig& config,
                                              const PythonSettings& settings,
                                              common::Logger& logger, std::string* error);
    ~Interpreter();

    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

    // Queues one request line (FIFO -- the driver's queue IS the ordering)
    // and returns its id without blocking: the reader thread owns the write,
    // so a request larger than the pipe buffer never parks the caller behind
    // a running request. `async` decides delivery:
    //   async: the reader injects the result keyed to handles.tool_use_id;
    //   sync:  WaitSync fulfills it (until DEMOTED, after which the reader
    //          takes over delivery exactly like an async request).
    // Background-task lifecycle events are emitted only for async requests
    // (from here, before the request can possibly complete) and for sync
    // ones once demoted -- a foreground wait is not a background task.
    // nullopt with *error set when the interpreter is dead.
    // `max_runtime_seconds` arms the request's own kill deadline, counted
    // from when it starts RUNNING (queue time excluded); 0 = unlimited.
    // `queued_ahead` (optional) receives how many requests run before it.
    std::optional<std::string> Submit(const std::string& code, bool async,
                                      RequestHandles handles, std::string* error,
                                      std::int64_t max_runtime_seconds = 0,
                                      std::size_t* queued_ahead = nullptr);

    enum class WaitOutcome { kCompleted, kDemoted, kDead, kCancelled };

    // Blocks a sync request up to `deadline`, also waking on cancellation.
    // NEVER kills anything:
    //   kCompleted  -- *out carries the result;
    //   kDemoted    -- the deadline (or CancelTurn) expired first; the request
    //                  continues and the reader will inject its result later;
    //   kDead       -- the interpreter died while waiting;
    //   kCancelled  -- the turn was cancelled; identical to kDemoted for the
    //                  interpreter, but the caller words its result honestly.
    WaitOutcome WaitSync(const std::string& id, const tools::CancelTokenPtr& cancel,
                         std::chrono::steady_clock::time_point deadline, RequestOutcome* out);

    // Soft interrupt: SIGINT to the process group. The running request fails
    // with KeyboardInterrupt (the driver catches per-request) and replies; the
    // interpreter and its state survive. Returns after sending.
    void SoftInterrupt();

    // The two counters WaitForInterrupt compares against, captured as one
    // unit so a caller can snapshot them BEFORE calling SoftInterrupt.
    // Capturing the baseline INSIDE WaitForInterrupt (after the signal was
    // already sent) raced a driver that replies within microseconds: the
    // bump could land in the gap between SoftInterrupt returning and
    // WaitForInterrupt's own snapshot, so it never observed a change and
    // reported a real, delivered interrupt as kNoReaction.
    struct ProgressSnapshot {
        std::uint64_t progress = 0;
        std::uint64_t idle_interrupts = 0;
    };
    ProgressSnapshot Progress() const;

    // Soft-interrupt observability: waits up to `grace` for the SIGINT to
    // land, also waking on cancellation. The driver reports which of three
    // things happened: the running request failed and replied (kResponse --
    // the demonstrable interrupt), the signal landed with NOTHING running
    // (kIdleInterrupt -- the driver spends it and says so), or nothing was
    // heard (kNoReaction -- likely a non-interruptible C extension).
    // `baseline` must be captured via Progress() BEFORE calling
    // SoftInterrupt(), not after -- see ProgressSnapshot's comment.
    enum class InterruptOutcome { kResponse, kIdleInterrupt, kNoReaction };
    InterruptOutcome WaitForInterrupt(const ProgressSnapshot& baseline,
                                      std::chrono::milliseconds grace,
                                      const tools::CancelTokenPtr& cancel);

    // Feeds the user-stdin pipe (the driver's sys.stdin reads it; python_input
    // is the tool surface). Non-blocking: returns the bytes accepted -- a full
    // pipe (the script is not consuming) yields a partial write and *error
    // set, never a parked session worker.
    std::size_t WriteStdin(const std::string& text, std::string* error);

    // Hard kill: SIGKILL + reap, idempotent. Every pending request fails
    // (async ones get injected error results when the session is still alive;
    // sync waits are woken with a dead outcome). The reader thread notices
    // and exits.
    void HardKill(const std::string& reason);

    // One-time notice gates for the tool's per-exec advisories (declared
    // conflict, bare-interpreter drift). True the FIRST call with a key,
    // false afterwards; resets only when a fresh interpreter is spawned.
    bool NoticeShown(const std::string& key);

    // The DECLARED contract this interpreter runs under: the preset slice at
    // start, plus anything auto-ensured since (AdoptDeclared). The per-exec
    // diff compares against THIS, so a successful ensure must fold its
    // entries in -- or every later call would re-run pip/uv.
    std::vector<std::string> Declared();
    void AdoptDeclared(const std::vector<std::string>& added);

    // python_output's data plane: the task listing, and one task's output.
    std::vector<TaskStatus> TaskStatuses();
    OutputRead ReadTaskOutput(const std::string& id, std::uint64_t offset, std::size_t length);

    bool alive() const { return !dead_.load(std::memory_order_acquire); }
    pid_t pid() const { return pid_; }
    const std::string& version() const { return version_; }
    const InterpreterConfig& config() const { return config_; }

    // Requests queued or running (for the tools' `pending` reporting and
    // /python). Oldest first; the first is the one currently running.
    std::vector<std::string> PendingIds();

    // The same rows with the /python-visible extras (armed deadline, live
    // stream size).
    struct PendingRow {
        std::string id;
        std::int64_t max_runtime = 0;
        std::uint64_t stream_bytes = 0;
    };
    std::vector<PendingRow> PendingRows();

private:
    struct Pending;  // defined below; method declarations reference it by pointer

    Interpreter(LaunchedInterpreter proc, InterpreterConfig config,
                const PythonSettings& settings, common::Logger& logger);
    void StartReader();
    void ReaderLoop();
    void MarkDead(std::string reason, bool session_dead);
    void DeliverResponse(const std::string& id, RequestOutcome outcome,
                         std::uint64_t streamed);
    // False when the line was fatal (the interpreter is dead).
    bool DrainLine(std::string_view line);
    void InjectAsync(const Pending& p);
    void EmitEvent(const RequestHandles& handles, const std::string& id,
                   tools::BackgroundTaskEvent::State state) const;
    // Wakes the reader out of poll() (a queued request, a kill).
    void Wake();
    // The pipe readers. Each returns false on EOF/error (channel closed).
    bool ReadStream();
    bool ReadDiag();
    // Reads up to `want` more live-stream bytes for `p`, waiting briefly for
    // bytes the driver has already written (see DeliverResponse).
    void ReadStreamOwed(const std::shared_ptr<Pending>& p, std::uint64_t want);
    void AttributeStreamLocked(std::string_view bytes);
    void AttributeDiagLocked(std::string_view bytes);

    struct Pending {
        std::string id;
        std::uint64_t seq = 0;  // submission order (the map itself is keyed, not FIFO)
        bool async = false;
        RequestHandles handles;
        // Fulfilled by the reader under mu_ before done=true. The waiter
        // holds its own shared_ptr, so the reader may erase the map entry
        // while a wait is parked on this object.
        RequestOutcome outcome;
        bool done = false;
        // Live-output ring (running request only in practice; queued ones
        // have produced no output) + the deadline machinery. The ring also
        // carries attributed process output; `streamed` counts only the
        // fd-4 bytes, the unit of the driver's per-response `streamed` count.
        StreamRing ring;
        std::uint64_t streamed = 0;
        std::string process_output;  // capped at max_capture_bytes
        std::uint64_t process_dropped = 0;
        std::int64_t max_runtime_seconds = 0;  // armed at run start (Submit's argument)
        std::optional<std::chrono::steady_clock::time_point> running_since;
        std::optional<std::chrono::steady_clock::time_point> deadline;
        bool deadline_fired = false;
        std::optional<std::chrono::steady_clock::time_point> deadline_fired_at;

        Pending(std::size_t head_cap, std::size_t tail_cap) : ring(head_cap, tail_cap) {}
    };

    // The not-done pending with the smallest seq -- the one whose code is
    // running (or about to run) in the driver. Caller holds mu_.
    std::shared_ptr<Pending> RunningPendingLocked();
    // Arms/refreshes running_since lazily (call sites: stream attribution,
    // deadline checks). Caller holds mu_.
    void StampRunningLocked(const std::shared_ptr<Pending>& p,
                            std::chrono::steady_clock::time_point now);

    InterpreterConfig config_;
    PythonSettings settings_;
    common::Logger& logger_;

    // The pid for display (logs, events) only: it stays readable after the child
    // is gone. Everything that signals, kills or reaps goes through `child_`.
    pid_t pid_ = -1;
    // Owns the process group's end of life. HardKill can run more than once for
    // one interpreter (a reader-thread death detection, an explicit /python
    // restart, ~Interpreter's own teardown), and Child::Terminate claims the pid
    // with an atomic exchange, so exactly one call kills and reaps it -- a pid
    // the OS is free to have recycled by the time a second call arrives is never
    // signalled.
    minimal::sidecar::Child child_;
    ScopedFd in_fd_;      // parent -> child (requests)
    ScopedFd out_fd_;     // child -> parent (responses)
    ScopedFd diag_fd_;    // child's real stdout/stderr, drained + kept for the death report
    ScopedFd stream_fd_;  // child -> parent (live stdout of the running request)
    ScopedFd user_in_fd_; // parent -> child (user stdin for executed code)
    ScopedFd wake_r_;     // self-pipe: Submit/HardKill wake the reader's poll
    ScopedFd wake_w_;
    // Protocol-line ceiling, derived from max_capture_bytes (every free-text
    // field the driver sends is capped at that many characters).
    std::size_t max_line_bytes_ = 0;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Pending>> pending_;
    // The last kFinishedRing completions (any delivery path), so
    // python_output can see a task through the completion->injection gap.
    struct FinishedEntry {
        std::string id;
        RequestOutcome outcome;
        double seconds = 0;  // total runtime
    };
    std::deque<FinishedEntry> finished_;
    // Request lines not yet written to the driver's stdin; the reader writes
    // them (non-blocking) as the pipe drains. Guarded by mu_.
    std::string outbox_;
    std::uint64_t next_seq_ = 0;   // submission-order source, guarded by mu_
    std::uint64_t progress_{0};        // bumped per delivered response or idle-interrupt
    std::uint64_t idle_interrupts_{0};  // monotonically bumped when the driver spends a SIGINT
                                        // with nothing running (counter, not a flag: a flag
                                        // would make the SECOND idle interrupt read as a
                                        // response -- start==end on a sticky bool)
    std::vector<std::string> declared_;  // mutable via AdoptDeclared, guarded by mu_
    std::vector<std::string> notices_;   // one-time notice gates, guarded by mu_
    bool ready_ = false;
    std::string version_;
    std::string diag_tail_;  // last diagnostics seen, reported on death
    std::atomic<bool> dead_{false};
    // JOINED in the destructor; holds no reference to the interpreter.
    std::thread reader_;
};

// The finished-completions ring depth: how far back `python_output` can see a
// completed task before its entry is forgotten. Completion records are tiny
// (the capped capture already rode the response line), so this bounds only
// metadata, not the output text.
inline constexpr std::size_t kFinishedRing = 8;

// Formats one completed request into the text the model sees (shared by the
// sync result and the async injection, so both audiences read the same shape).
// `queued_ahead` is how many requests ran before this one (0 = none).
std::string FormatOutcome(const std::string& id, const RequestOutcome& outcome,
                          std::size_t queued_ahead, std::int64_t max_bytes);

}  // namespace minimal::tools_python
