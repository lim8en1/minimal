#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
    std::string python_path;             // absolute interpreter binary
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
    std::string error;        // transport-level failure description when !ok
    // Structured-return fields from the driver: the exception class + message
    // on a failed request, and the JSON rendering of a serializable trailing
    // expression on a successful one (empty when the value did not serialize).
    std::string exc_type;
    std::string exc_msg;
    std::string result_json;
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
// or an external kill all converge on "dead" -- pending async requests get
// injected error results, pending sync waits are woken with a dead outcome,
// and the next python_exec auto-restarts.
//
// THREAD/OWNERSHIP CONTRACT: the reader runs JOINABLE and is joined in the
// destructor after a HardKill (EOF wakes its poll within one slice). It holds
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

    // Writes one request line (FIFO -- the driver's queue IS the ordering).
    // The id is returned; deliver decides sync-vs-async delivery:
    //   async: the reader injects the result keyed to handles.tool_use_id;
    //   sync:  WaitSync fulfills it (until DEMOTED, after which the reader
    //          takes over delivery exactly like an async request).
    // nullopt with *error set when the interpreter is dead or the pipe
    // refuses the write. `max_runtime_seconds` arms the request's own kill
    // deadline, counted from when it starts RUNNING (queue time excluded);
    // 0 = unlimited.
    std::optional<std::string> Submit(const std::string& code, bool async,
                                      RequestHandles handles, std::string* error,
                                      std::int64_t max_runtime_seconds = 0);

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

    // Soft-interrupt observability: waits up to `grace` for the SIGINT to
    // land, also waking on cancellation. The driver reports which of three
    // things happened: the running request failed and replied (kResponse --
    // the demonstrable interrupt), the signal landed with NOTHING running
    // (kIdleInterrupt -- the driver spends it and says so), or nothing was
    // heard (kNoReaction -- likely a non-interruptible C extension).
    enum class InterruptOutcome { kResponse, kIdleInterrupt, kNoReaction };
    InterruptOutcome WaitForInterrupt(std::chrono::milliseconds grace,
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
    void HardKill(const char* reason);

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
    void DeliverResponse(const std::string& id, RequestOutcome outcome);
    void DrainLine(std::string_view line);
    void InjectAsync(const Pending& p);

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
        // have produced no output) + the deadline machinery.
        StreamRing ring;
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

    pid_t pid_ = -1;
    ScopedFd in_fd_;      // parent -> child (requests)
    ScopedFd out_fd_;     // child -> parent (responses)
    ScopedFd diag_fd_;    // child's real stdout/stderr, drained + kept for the death report
    ScopedFd stream_fd_;  // child -> parent (live stdout of the running request)
    ScopedFd user_in_fd_; // parent -> child (user stdin for executed code)

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
std::string FormatOutcome(const std::string& id, const RequestOutcome& outcome,
                          std::size_t queued_behind, std::int64_t max_bytes);

}  // namespace minimal::tools_python
