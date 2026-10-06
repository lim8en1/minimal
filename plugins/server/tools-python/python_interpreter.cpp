#include "python_interpreter.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include <utility>

#include "minimal/common/utf8.hpp"
#include "minimal/common/units.hpp"

#include "driver_source.hpp"
#include "json_fields.hpp"
#include "policy.hpp"
#include "minimal/common/policy.hpp"
#include "minimal/sidecar/policy.hpp"

namespace minimal::tools_python {
namespace {

using tools::BackgroundTaskEvent;

// The reader's slice. Everything that needs the reader promptly (a queued
// request, a kill, protocol traffic) wakes poll() directly, and deadlines
// shorten the slice; the slice itself only bounds how late a session's death
// is noticed through session_lifetime -- OnSessionEnded's Erase already
// kills, so this is the backstop, not the mechanism (kReaderPollSlice).
// Live-stream bytes the driver reports having written are already in the pipe
// (the driver writes them before the response line), so kStreamOwedBudget only
// bounds a driver whose count was off (a signal between write and count).
constexpr std::size_t kReadChunk = common::kReadChunkBytes;  // stack buffers; the reader nests three

// Sync waiters poll the cancel flag on this slice while parked on the
// interpreter cv (two cvs -- the token's and the interpreter's -- cannot be
// waited on together; a slice bounds cancel latency without a second
// registration mechanism): common::kCancelPollSlice.

// Poll timeout bounded by the nearest armed request deadline, clamped to the
// slice: deadline enforcement never waits longer than one slice past expiry.
int BoundedSlice(int slice_ms, std::chrono::steady_clock::duration until) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until).count();
    if (ms <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(slice_ms, ms));
}

// The protocol-line ceiling. The driver caps each free-text field (result,
// stdout, stderr, exc_msg) at max_capture_bytes CHARACTERS, and json.dumps
// ASCII-escapes: an astral code point is 12 bytes on the wire (two \uXXXX
// escapes). A fixed limit let a raised max_capture_bytes turn ordinary
// output into "corruption"; this derives it from the same knob.
std::size_t MaxLineBytes(std::int64_t max_capture_bytes) {
    // NOLINTBEGIN(readability-magic-numbers) -- the protocol's shape: four
    // free-text fields, and an astral code point json.dumps-escaped as two
    // \uXXXX sequences.
    constexpr std::size_t kFields = 4, kBytesPerChar = 12;
    // NOLINTEND(readability-magic-numbers)
    const auto cap = static_cast<std::size_t>(std::max<std::int64_t>(max_capture_bytes, 0));
    return std::max(kMinProtocolLineBytes,
                    kFields * kBytesPerChar * (cap + kProtocolFieldSlackChars) +
                        kProtocolEnvelopeBytes);
}

void SetNonBlocking(int fd) {
    if (fd < 0) return;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

Interpreter::Interpreter(LaunchedInterpreter proc, InterpreterConfig config,
                         const PythonSettings& settings, common::Logger& logger)
    : config_(std::move(config)),
      settings_(settings),
      logger_(logger),
      pid_(proc.pid),
      child_(proc.pid),
      in_fd_(proc.in_fd),
      out_fd_(proc.out_fd),
      diag_fd_(proc.diag_fd),
      stream_fd_(proc.stream_fd),
      user_in_fd_(proc.user_in_fd),
      max_line_bytes_(MaxLineBytes(settings.max_capture_bytes)),
      declared_(config_.declared) {
    // Every channel is non-blocking: the reader drains stream/diag to EAGAIN
    // before handling a response, writes queued requests as the pipe drains,
    // and python_input never parks the session worker on a full stdin pipe.
    SetNonBlocking(in_fd_.get());
    SetNonBlocking(stream_fd_.get());
    SetNonBlocking(diag_fd_.get());
    SetNonBlocking(user_in_fd_.get());
    int wake[2];
    if (::pipe(wake) == 0) {
        wake_r_ = ScopedFd(wake[0]);
        wake_w_ = ScopedFd(wake[1]);
        for (const int fd : wake) {
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            SetNonBlocking(fd);
        }
    }
}

Interpreter::~Interpreter() {
    HardKill("interpreter destroyed");
    if (reader_.joinable()) reader_.join();
}

std::shared_ptr<Interpreter> Interpreter::Spawn(const InterpreterConfig& config,
                                                const PythonSettings& settings,
                                                common::Logger& logger, std::string* error) {
    std::vector<std::string> argv{config.python_path, "-I", "-c", kDriverSource};
    LaunchedInterpreter proc =
        LaunchInterpreter(argv, config.cwd, config.env, settings.rlimit_cpu_seconds,
                          settings.max_file_size_bytes);
    if (!proc.ok) {
        *error = "failed to start " + config.python_path + ": " + proc.spawn_error;
        return nullptr;
    }
    // Degraded backstops (host caps RLIMIT_* below the request) still start:
    // say so, or a capped host's interpreters run silently weaker than asked.
    if (!proc.warnings.empty()) {
        logger.warn("python[{}]: rlimit backstops clamped by the host: {}", proc.pid,
                    proc.warnings);
    }

    // The interpreter owns the reader thread JOINABLY: the destructor kills
    // the process first (EOF wakes the reader) and joins before any member
    // dies, so the reader can safely touch members up to the moment join()
    // returns.
    std::shared_ptr<Interpreter> self(
        new Interpreter(std::move(proc), config, settings, logger));
    self->StartReader();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(settings.startup_timeout);
    bool ready = false;
    std::string diag;
    {
        std::unique_lock<std::mutex> lock(self->mu_);
        while (!self->ready_ && !self->dead_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= deadline) break;
            self->cv_.wait_until(lock, deadline);
        }
        // Read under the same lock the reader thread writes both fields
        // under: the reader is still running here regardless of which way the
        // wait ended, so an unlocked read races it.
        ready = self->ready_;
        diag = self->diag_tail_;
    }
    if (!ready) {
        self->HardKill("startup handshake timed out");
        *error = "interpreter did not become ready within " +
                 std::to_string(settings.startup_timeout) + "s (broken venv or interpreter?)" +
                 (diag.empty() ? "" : "\n" + diag);
        return nullptr;
    }
    return self;
}

void Interpreter::StartReader() {
    reader_ = std::thread([this] { ReaderLoop(); });
}

void Interpreter::Wake() {
    if (wake_w_.get() < 0) return;
    const char byte = 1;
    // A full wake pipe already guarantees a wakeup; EAGAIN is success.
    [[maybe_unused]] const auto n = ::write(wake_w_.get(), &byte, 1);
}

void Interpreter::EmitEvent(const RequestHandles& handles, const std::string& id,
                            BackgroundTaskEvent::State state) const {
    if (!handles.emit_task_event) return;
    try {
        BackgroundTaskEvent event;
        event.task_id = id;
        event.tool_use_id = handles.tool_use_id;
        event.command = handles.label;
        event.pid = pid_;
        event.state = state;
        handles.emit_task_event(event);
    } catch (...) {
    }
}

void Interpreter::AttributeStreamLocked(std::string_view bytes) {
    // FIFO: at most one request runs, and it is the not-done head. Bytes with
    // no running request are the tail of an already-reconciled request whose
    // count was off -- dropped (the final capture is the record).
    auto p = RunningPendingLocked();
    if (!p) return;
    p->streamed += bytes.size();
    p->ring.Append(bytes);
    if (p->handles.emit_output) {
        try {
            p->handles.emit_output(bytes);
        } catch (...) {
        }
    }
}

void Interpreter::AttributeDiagLocked(std::string_view bytes) {
    diag_tail_.append(bytes);
    if (diag_tail_.size() > kMaxDiagBytes) {
        diag_tail_.erase(0, diag_tail_.size() - kMaxDiagBytes);
    }
    // Process-level output (fd 1/2: subprocesses, os.system, C extensions)
    // while a request runs is that request's. Best-effort: a child that
    // outlives its request writes into whatever runs next.
    auto p = RunningPendingLocked();
    if (!p) return;
    const auto cap = static_cast<std::size_t>(settings_.max_capture_bytes);
    if (p->process_output.size() < cap) {
        const auto take = std::min(cap - p->process_output.size(), bytes.size());
        p->process_output.append(bytes.substr(0, take));
        p->process_dropped += bytes.size() - take;
    } else {
        p->process_dropped += bytes.size();
    }
    p->ring.Append(bytes);
    if (p->handles.emit_output) {
        try {
            p->handles.emit_output(bytes);
        } catch (...) {
        }
    }
}

bool Interpreter::ReadStream() {
    char chunk[kReadChunk];
    while (true) {
        const ssize_t n = ::read(stream_fd_.get(), chunk, sizeof(chunk));
        if (n > 0) {
            const std::lock_guard<std::mutex> lock(mu_);
            AttributeStreamLocked(std::string_view(chunk, static_cast<std::size_t>(n)));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;  // EOF or error
    }
}

bool Interpreter::ReadDiag() {
    char chunk[kReadChunk];
    while (true) {
        const ssize_t n = ::read(diag_fd_.get(), chunk, sizeof(chunk));
        if (n > 0) {
            const std::lock_guard<std::mutex> lock(mu_);
            AttributeDiagLocked(std::string_view(chunk, static_cast<std::size_t>(n)));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        return false;
    }
}

void Interpreter::ReadStreamOwed(const std::shared_ptr<Pending>& p, std::uint64_t want) {
    const auto deadline = std::chrono::steady_clock::now() + kStreamOwedBudget;
    char chunk[kReadChunk];
    while (want > 0) {
        const auto len = static_cast<std::size_t>(std::min<std::uint64_t>(want, sizeof(chunk)));
        const ssize_t n = ::read(stream_fd_.get(), chunk, len);
        if (n > 0) {
            const std::lock_guard<std::mutex> lock(mu_);
            const std::string_view bytes(chunk, static_cast<std::size_t>(n));
            p->streamed += bytes.size();
            p->ring.Append(bytes);
            if (p->handles.emit_output) {
                try {
                    p->handles.emit_output(bytes);
                } catch (...) {
                }
            }
            want -= static_cast<std::uint64_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return;
        const auto left = deadline - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return;
        pollfd fd{stream_fd_.get(), POLLIN, 0};
        ::poll(&fd, 1, BoundedSlice(static_cast<int>(sidecar::kDrainSlice.count()), left));
    }
}

void Interpreter::ReaderLoop() {
    std::string line;
    bool out_open = true, diag_open = true, stream_open = true;
    while (out_open || diag_open || stream_open) {
        if (dead_.load(std::memory_order_acquire)) return;  // HardKill cleaned up already
        if (config_.session_lifetime.expired()) {
            // Session terminate: the backstop kill path (OnSessionEnded's
            // Erase normally gets there first).
            HardKill("session ended");
            return;
        }

        // The request-deadline scan (max_runtime). The deadline is armed at
        // RUN start (queue time excluded); expiry first soft-interrupts --
        // the request may still finish and deliver a real result -- and a
        // request that ignores SIGINT for grace_seconds is hard-killed. This
        // is the plugin's ONE destructive timer, and only for requests that
        // asked for it.
        const auto now = std::chrono::steady_clock::now();
        int timeout = static_cast<int>(kReaderPollSlice.count());
        std::shared_ptr<Pending> fire, kill;
        bool want_write = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            want_write = !outbox_.empty();
            if (auto p = RunningPendingLocked()) {
                StampRunningLocked(p, now);
                if (p->deadline) {
                    if (!p->deadline_fired && now >= *p->deadline) {
                        p->deadline_fired = true;
                        p->deadline_fired_at = now;
                        fire = p;
                        if (p->async) EmitEvent(p->handles, p->id, BackgroundTaskEvent::State::kTimedOut);
                    } else if (p->deadline_fired) {
                        const auto grace_end =
                            *p->deadline_fired_at + std::chrono::seconds(settings_.grace_seconds);
                        if (now >= grace_end) {
                            kill = p;
                        } else {
                            timeout = BoundedSlice(timeout, grace_end - now);
                        }
                    } else {
                        timeout = BoundedSlice(timeout, *p->deadline - now);
                    }
                }
            }
        }
        if (kill) {
            HardKill("request " + kill->id + " exceeded its max_runtime deadline and did not " +
                     "react to the interrupt within " + std::to_string(settings_.grace_seconds) +
                     "s; the interpreter was killed (state lost) to stop it");
            return;
        }
        if (fire) SoftInterrupt();

        pollfd fds[5];  // NOLINT(readability-magic-numbers): one per channel added below
        nfds_t nfds = 0;
        auto add = [&](bool on, int fd, short events) -> int {
            if (!on || fd < 0) return -1;
            fds[nfds] = pollfd{fd, events, 0};
            return static_cast<int>(nfds++);
        };
        const int out_slot = add(out_open, out_fd_.get(), POLLIN);
        const int diag_slot = add(diag_open, diag_fd_.get(), POLLIN);
        const int stream_slot = add(stream_open, stream_fd_.get(), POLLIN);
        const int in_slot = add(want_write, in_fd_.get(), POLLOUT);
        const int wake_slot = add(true, wake_r_.get(), POLLIN);

        const int rc = ::poll(fds, nfds, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            HardKill("interpreter pipe error");
            return;
        }
        auto ready = [&](int slot) {
            return slot >= 0 && (fds[slot].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR));
        };

        if (ready(wake_slot)) {
            char sink[64];  // NOLINT(readability-magic-numbers): any size drains the wake pipe
            while (::read(wake_r_.get(), sink, sizeof(sink)) > 0) {
            }
        }
        if (ready(in_slot)) {
            std::lock_guard<std::mutex> lock(mu_);
            while (!outbox_.empty()) {
                const ssize_t n = ::write(in_fd_.get(), outbox_.data(), outbox_.size());
                if (n > 0) {
                    outbox_.erase(0, static_cast<std::size_t>(n));
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // The driver closed its stdin: it is dying; the EOF on the
                    // protocol pipe reports it. Nothing more can be written.
                    outbox_.clear();
                }
                break;
            }
        }
        if (ready(stream_slot) && !ReadStream()) stream_open = false;
        if (ready(diag_slot) && !ReadDiag()) diag_open = false;
        if (ready(out_slot)) {
            char chunk[kReadChunk];
            const ssize_t n = ::read(out_fd_.get(), chunk, sizeof(chunk));
            if (n > 0) {
                line.append(chunk, static_cast<std::size_t>(n));
                std::size_t start = 0;
                while (true) {
                    const auto pos = line.find('\n', start);
                    if (pos == std::string::npos) break;
                    // DrainLine reads through the non-throwing field helpers;
                    // this guard is a second line of defense: the reader has
                    // nowhere to send an exception, and an uncaught throw
                    // here would std::terminate the whole server over one
                    // malformed protocol line.
                    bool alive = false;
                    try {
                        alive = DrainLine(std::string_view(line).substr(start, pos - start));
                    } catch (const std::exception& error) {
                        logger_.warn("python[{}]: DrainLine threw: {}", pid_, error.what());
                        HardKill("protocol corruption");
                    }
                    if (!alive) return;
                    start = pos + 1;
                }
                line.erase(0, start);
                if (line.size() > max_line_bytes_) {
                    HardKill("interpreter produced an oversized protocol line");
                    return;
                }
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                // EOF: the process exited on its own (crash, os._exit, kill).
                // Drain what it left on the diagnostics pipe for the report.
                if (diag_open) ReadDiag();
                std::string tail;
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    tail = diag_tail_;
                }
                HardKill("interpreter exited" + (tail.empty() ? "" : ": " + tail));
                return;
            }
        }
    }
}

bool Interpreter::DrainLine(std::string_view line) {
    if (line.empty()) return true;
    nlohmann::json body = nlohmann::json::parse(line, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        logger_.warn("python[{}]: undecodable protocol line ({} bytes)", pid_, line.size());
        HardKill("protocol corruption");
        return false;
    }
    if (body.contains("ready")) {
        std::lock_guard<std::mutex> lock(mu_);
        version_ = StringField(body, "version", "unknown");
        ready_ = true;
        cv_.notify_all();
        return true;
    }
    if (body.contains("idle_interrupted")) {
        // A SIGINT landed with nothing running: the driver spent it and keeps
        // serving. Counted so the interrupt tool can say "nothing was
        // running" instead of guessing at a wedged C extension. A counter, so
        // every idle interrupt is distinguishable from its predecessors.
        std::lock_guard<std::mutex> lock(mu_);
        ++idle_interrupts_;
        ++progress_;
        cv_.notify_all();
        return true;
    }
    const auto id_it = body.find("id");
    const bool has_id = id_it != body.end() && id_it->is_string();
    if (body.contains("bad_request")) {
        const auto why = StringField(body, "bad_request");
        logger_.warn("python[{}]: driver rejected a request: {}", pid_, why);
        // With an id the request can be failed; without one it cannot be
        // matched (the parent only ever sends well-formed lines).
        if (has_id) {
            RequestOutcome outcome;
            outcome.error = "the interpreter rejected the request: " + why;
            DeliverResponse(id_it->get<std::string>(), std::move(outcome), 0);
        }
        return !dead_.load(std::memory_order_acquire);
    }
    if (!has_id) {
        logger_.warn("python[{}]: protocol line without an id; ignoring", pid_);
        return true;
    }
    RequestOutcome outcome;
    outcome.ok = BoolField(body, "ok");
    // stdout rides BOTH response kinds: a request that ran and then failed
    // keeps everything it printed before the exception.
    outcome.stdout_text = StringField(body, "stdout");
    outcome.stderr_text = StringField(body, "stderr");
    if (outcome.ok) {
        outcome.result = StringField(body, "result");
    } else {
        outcome.error = StringField(body, "error", "request failed");
        outcome.exc_type = StringField(body, "exc");
        outcome.exc_msg = StringField(body, "exc_msg");
    }
    DeliverResponse(id_it->get<std::string>(), std::move(outcome),
                    UintField(body, "streamed"));
    return !dead_.load(std::memory_order_acquire);
}

void Interpreter::DeliverResponse(const std::string& id, RequestOutcome outcome,
                                  std::uint64_t streamed) {
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            logger_.warn("python[{}]: response for unknown request {}", pid_, id);
            return;
        }
        p = it->second;
    }

    // Stream reconciliation: `streamed` is exactly how many fd-4 bytes this
    // request wrote, all of them before this response line. Fewer read so
    // far: the rest is already in the pipe -- read it now, so the tail is not
    // attributed to the next request. More: the reader overshot into the
    // next request's first bytes (read before this line was) -- hand them on.
    // Process output (fd 1/2) is drained too; it has no count, so it stays
    // best-effort.
    if (p->streamed < streamed) ReadStreamOwed(p, streamed - p->streamed);
    ReadDiag();

    bool async = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // A kill that landed while the stream was reconciled (lock released
        // above) already failed and reported this request; delivering too
        // would report it twice.
        if (p->done) return;
        std::string overshoot;
        if (p->streamed > streamed) {
            overshoot = p->ring.TakeBack(p->streamed - streamed);
            p->streamed = streamed;
        }
        p->outcome = std::move(outcome);
        if (!p->process_output.empty() || p->process_dropped > 0) {
            p->outcome.process_output = p->process_output;
            if (p->process_dropped > 0) {
                p->outcome.process_output +=
                    "\n[... " + std::to_string(p->process_dropped) + " bytes truncated ...]";
            }
        }
        // The deadline flag rides the outcome: a result that landed despite
        // the SIGINT is real, but the model must know the deadline was felt.
        p->outcome.deadline_interrupted = p->deadline_fired;
        p->done = true;
        async = p->async;
        ++progress_;
        cv_.notify_all();
        // A SYNC request's result is CLAIMED by its WaitSync (which erases the
        // entry): the response can beat the waiter to the map, and erasing
        // here would lose it. Async entries are consumed here and now.
        if (async) pending_.erase(id);
        const auto now = std::chrono::steady_clock::now();
        const double seconds =
            p->running_since ? std::chrono::duration<double>(now - *p->running_since).count() : 0.0;
        finished_.push_back(FinishedEntry{p->id, p->outcome, seconds});
        if (finished_.size() > kFinishedRing) finished_.pop_front();
        if (!overshoot.empty()) AttributeStreamLocked(overshoot);
    }

    // Lifecycle events exist only for background (async or demoted) work;
    // Started went out under mu_ before this request could complete, so a
    // client never sees the terminal state first. A session that is gone
    // refuses the injection anyway -- skipping is equivalent.
    if (!async) return;
    EmitEvent(p->handles, p->id,
              p->outcome.ok ? BackgroundTaskEvent::State::kCompleted
                            : BackgroundTaskEvent::State::kFailed);
    if (!p->handles.session_lifetime.expired()) InjectAsync(*p);
}

void Interpreter::InjectAsync(const Pending& p) {
    const bool ok = p.outcome.ok;
    std::string text = FormatOutcome(p.id, p.outcome, 0, settings_.max_output_bytes);
    text = ok ? text + "\n[python " + p.id + " completed]"
              : "[python " + p.id + " failed]\n" + text;
    llm::ToolResultBlock block;
    block.tool_use_id = p.handles.tool_use_id;
    block.is_error = !ok;
    block.content.push_back(llm::TextBlock{std::move(text)});
    if (p.handles.schedule_injection &&
        !p.handles.schedule_injection(llm::Message{llm::Role::User, {std::move(block)}})) {
        logger_.warn("python[{}]: {} result dropped; session already terminating", pid_, p.id);
    }
}

std::optional<std::string> Interpreter::Submit(const std::string& code, bool async,
                                               RequestHandles handles, std::string* error,
                                               std::int64_t max_runtime_seconds,
                                               std::size_t* queued_ahead) {
    static std::atomic<std::uint64_t> next_id{1};
    const auto id = "py-" + std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
    auto payload = nlohmann::json{{"id", id}, {"code", code}}.dump(
        -1, ' ', false, nlohmann::json::error_handler_t::replace);
    payload.push_back('\n');

    {
        std::lock_guard<std::mutex> lock(mu_);
        // Checked under mu_: MarkDead sets dead_ and then clears pending_
        // under this lock, so an entry inserted after a death is impossible
        // and one inserted before it is failed by MarkDead.
        if (dead_.load(std::memory_order_acquire)) {
            *error = "interpreter is not running";
            return std::nullopt;
        }
        std::size_t ahead = 0;
        for (const auto& [other_id, other] : pending_) {
            if (!other->done) ++ahead;
        }
        if (queued_ahead != nullptr) *queued_ahead = ahead;
        auto p = std::make_shared<Pending>(static_cast<std::size_t>(settings_.stream_head_bytes),
                                           static_cast<std::size_t>(settings_.stream_tail_bytes));
        p->id = id;
        p->seq = next_seq_++;
        p->async = async;
        p->handles = std::move(handles);
        p->max_runtime_seconds = max_runtime_seconds > 0 ? max_runtime_seconds : 0;
        // Started goes out under mu_, BEFORE the request line can reach the
        // driver: the completion (emitted by the reader after it takes mu_)
        // can never overtake it.
        if (async) EmitEvent(p->handles, id, BackgroundTaskEvent::State::kStarted);
        pending_.emplace(id, std::move(p));
        outbox_ += payload;
    }
    Wake();
    return id;
}

Interpreter::WaitOutcome Interpreter::WaitSync(const std::string& id,
                                               const tools::CancelTokenPtr& cancel,
                                               std::chrono::steady_clock::time_point deadline,
                                               RequestOutcome* out) {
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_.find(id);
        if (it == pending_.end()) return WaitOutcome::kDead;
        p = it->second;  // the reader may erase the map entry; this object stays
    }
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
        if (p->done) {
            *out = p->outcome;
            // CLAIM: the result is consumed and the entry leaves the registry.
            pending_.erase(id);
            return WaitOutcome::kCompleted;
        }
        if (dead_.load(std::memory_order_acquire)) {
            // MarkDead has already filled every pending with a dead outcome
            // under mu_ before setting the flag's consumers loose.
            *out = p->outcome;
            return p->done ? WaitOutcome::kCompleted : WaitOutcome::kDead;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool expired = now >= deadline;
        const bool cancelled = !expired && cancel && cancel->IsCancelled();
        if (expired || cancelled) {
            // THE DEMOTION: the deadline (or CancelTurn, which never kills
            // background work) converts the request to async delivery -- the
            // interpreter and its state are untouched, and the reader injects
            // the result when it completes. Under mu_, so a response arriving
            // in the race window is either seen as done above or delivered
            // async; it is now a background task, announced as one.
            p->async = true;
            EmitEvent(p->handles, p->id, BackgroundTaskEvent::State::kStarted);
            return expired ? WaitOutcome::kDemoted : WaitOutcome::kCancelled;
        }
        cv_.wait_until(lock, std::min(deadline, now + common::kCancelPollSlice));
    }
}

void Interpreter::SoftInterrupt() { child_.Signal(SIGINT); }

std::shared_ptr<Interpreter::Pending> Interpreter::RunningPendingLocked() {
    // The not-done pending with the smallest submission seq: the driver is
    // strict FIFO, so its code is running (or is next to run). The map is
    // keyed, not ordered -- "py-10" sorts before "py-9" -- hence the seq scan.
    std::shared_ptr<Pending> best;
    for (auto& [id, p] : pending_) {
        if (p->done) continue;
        if (!best || p->seq < best->seq) best = p;
    }
    return best;
}

void Interpreter::StampRunningLocked(const std::shared_ptr<Pending>& p,
                                     std::chrono::steady_clock::time_point now) {
    if (p->running_since) return;
    p->running_since = now;
    if (p->max_runtime_seconds > 0) {
        // Clamped here too, not just at the parse sites: this is the actual
        // computation that overflows (seconds -> a nanosecond-resolution
        // duration), so no caller of Submit can wrap it into the past.
        const auto seconds = std::min(p->max_runtime_seconds, kMaxSaneRuntimeSeconds);
        p->deadline = now + std::chrono::seconds(seconds);
    }
}

std::size_t Interpreter::WriteStdin(const std::string& text, std::string* error) {
    if (text.empty()) return 0;
    if (dead_.load(std::memory_order_acquire)) {
        *error = "interpreter is not running";
        return 0;
    }
    std::size_t written = 0;
    while (written < text.size()) {
        const ssize_t n =
            ::write(user_in_fd_.get(), text.data() + written, text.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                *error = "stdin pipe full; the running code is not consuming input (" +
                         std::to_string(written) + " of " + std::to_string(text.size()) +
                         " bytes delivered)";
                return written;
            }
            *error = std::string("failed to write to the interpreter stdin: ") + std::strerror(errno);
            return written;
        }
        written += static_cast<std::size_t>(n);
    }
    return written;
}

std::vector<TaskStatus> Interpreter::TaskStatuses() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<TaskStatus> out;
    // Running + queued, in true submission order.
    std::vector<std::shared_ptr<Pending>> live;
    for (auto& [id, p] : pending_) {
        if (!p->done) live.push_back(p);
    }
    std::sort(live.begin(), live.end(),
              [](const auto& a, const auto& b) { return a->seq < b->seq; });
    for (std::size_t i = 0; i < live.size(); ++i) {
        const auto& p = live[i];
        TaskStatus ts;
        ts.id = p->id;
        ts.max_runtime = p->max_runtime_seconds;
        if (i == 0) {
            ts.state = TaskStatus::State::kRunning;
            StampRunningLocked(p, now);
            ts.seconds = p->running_since
                             ? std::chrono::duration<double>(now - *p->running_since).count()
                             : 0.0;
            ts.stream_total = p->ring.total();
        } else {
            ts.state = TaskStatus::State::kQueued;
            ts.queued_behind = i;
        }
        out.push_back(std::move(ts));
    }
    // Finished (the completion->injection window; bounded ring).
    for (const auto& f : finished_) {
        TaskStatus ts;
        ts.id = f.id;
        ts.state = TaskStatus::State::kFinished;
        ts.ok = f.outcome.ok;
        ts.seconds = f.seconds;
        out.push_back(std::move(ts));
    }
    return out;
}

OutputRead Interpreter::ReadTaskOutput(const std::string& id, std::uint64_t offset,
                                       std::size_t length) {
    std::lock_guard<std::mutex> lock(mu_);
    OutputRead out;
    if (auto it = pending_.find(id); it != pending_.end() && !it->second->done) {
        out.found = true;
        out.slice = it->second->ring.Read(offset, length);
        return out;
    }
    for (const auto& f : finished_) {
        if (f.id != id) continue;
        out.found = true;
        out.finished = true;
        auto& s = out.slice;
        s.total = f.outcome.stdout_text.size();
        if (offset < s.total) {
            const auto end = std::min(offset + static_cast<std::uint64_t>(length), s.total);
            s.text = f.outcome.stdout_text.substr(static_cast<std::size_t>(offset),
                                                  static_cast<std::size_t>(end - offset));
            s.next = end;
        } else {
            s.next = s.total;
        }
        return out;
    }
    return out;
}

Interpreter::ProgressSnapshot Interpreter::Progress() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {progress_, idle_interrupts_};
}

Interpreter::InterruptOutcome Interpreter::WaitForInterrupt(
    const ProgressSnapshot& baseline, std::chrono::milliseconds grace,
    const tools::CancelTokenPtr& cancel) {
    std::unique_lock<std::mutex> lock(mu_);
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (true) {
        if (progress_ != baseline.progress) {
            return idle_interrupts_ != baseline.idle_interrupts ? InterruptOutcome::kIdleInterrupt
                                                                 : InterruptOutcome::kResponse;
        }
        if (dead_.load(std::memory_order_acquire)) return InterruptOutcome::kNoReaction;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return InterruptOutcome::kNoReaction;
        if (cancel && cancel->IsCancelled()) return InterruptOutcome::kNoReaction;
        cv_.wait_until(lock, std::min(deadline, now + common::kCancelPollSlice));
    }
}

bool Interpreter::NoticeShown(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    if (std::find(notices_.begin(), notices_.end(), key) != notices_.end()) return false;
    notices_.push_back(key);
    return true;
}

std::vector<std::string> Interpreter::Declared() {
    std::lock_guard<std::mutex> lock(mu_);
    return declared_;
}

void Interpreter::AdoptDeclared(const std::vector<std::string>& added) {
    std::lock_guard<std::mutex> lock(mu_);
    declared_.insert(declared_.end(), added.begin(), added.end());
}

void Interpreter::MarkDead(std::string reason, bool session_dead) {
    bool expected = false;
    if (!dead_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    std::vector<std::shared_ptr<Pending>> to_fail;
    {
        std::lock_guard<std::mutex> lock(mu_);
        outbox_.clear();
        for (auto it = pending_.begin(); it != pending_.end();) {
            auto& p = it->second;
            // A done-but-unclaimed entry (a sync result DeliverResponse filled
            // whose WaitSync has not run yet -- this death can predate the
            // caller by any amount) stays in the map, findable, with its REAL
            // outcome. Only NOT-done entries become failures and leave.
            if (p->done) {
                ++it;
                continue;
            }
            // Filled UNDER mu_: sync waiters read these fields under it.
            p->outcome = RequestOutcome{};
            p->outcome.error = reason;
            p->done = true;
            to_fail.push_back(p);
            it = pending_.erase(it);
        }
        ++progress_;
        cv_.notify_all();
    }
    Wake();
    for (const auto& p : to_fail) {
        if (session_dead) continue;  // the session is gone; nothing to report to
        // Only ASYNC requests are reported here: a SYNC request's tool call
        // is the delivery (WaitSync wakes on the filled outcome), and a
        // second result for the same tool_use_id is rejected by the provider.
        if (!p->async) continue;
        EmitEvent(p->handles, p->id, BackgroundTaskEvent::State::kFailed);
        InjectAsync(*p);
    }
}

void Interpreter::HardKill(const std::string& reason) {
    MarkDead(reason, config_.session_lifetime.expired());
    // Idempotent the same way MarkDead is: Child::Terminate claims the pid
    // atomically, so a second HardKill (session teardown racing an explicit
    // kill, ~Interpreter after an earlier death) finds nothing to do instead of
    // re-killing a pid the OS may have recycled since. An unreaped child
    // (D state) is logged, never waited on forever.
    if (!child_.Terminate()) {
        logger_.warn("python[{}]: child not reaped within the post-kill budget ({})",
                     pid_, reason);
    }
    // The reader exits on its next wakeup (dead_ is set, Wake() fired). It
    // holds NO shared_ptr -- it captures raw `this` (StartReader) -- so it is
    // safe only because ~Interpreter joins it before any member is torn down.
}

std::vector<std::string> Interpreter::PendingIds() {
    std::lock_guard<std::mutex> lock(mu_);
    // Submission order, not map order ("py-10" sorts before "py-9").
    std::vector<std::pair<std::uint64_t, std::string>> ordered;
    for (const auto& [id, p] : pending_) {
        // Done-but-unclaimed sync results are not "pending" work.
        if (!p->done) ordered.emplace_back(p->seq, id);
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::string> ids;
    ids.reserve(ordered.size());
    for (auto& [seq, id] : ordered) ids.push_back(std::move(id));
    return ids;
}

std::vector<Interpreter::PendingRow> Interpreter::PendingRows() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<std::pair<std::uint64_t, const Pending*>> ordered;
    for (const auto& [id, p] : pending_) {
        if (!p->done) ordered.emplace_back(p->seq, p.get());
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<PendingRow> rows;
    rows.reserve(ordered.size());
    for (const auto& [seq, p] : ordered) {
        rows.push_back(PendingRow{p->id, p->max_runtime_seconds, p->ring.total()});
    }
    return rows;
}

std::string FormatOutcome(const std::string& id, const RequestOutcome& outcome,
                          std::size_t queued_ahead, std::int64_t max_bytes) {
    std::string text;
    if (outcome.ok) {
        text = id + (outcome.result.empty() ? " ok" : " ok: " + outcome.result);
    } else {
        text = id + " failed";
        if (!outcome.exc_type.empty()) {
            text = id + " exception: " + outcome.exc_type;
            if (!outcome.exc_msg.empty()) {
                // First line only -- the traceback lives under --- stderr ---.
                text += ": " + outcome.exc_msg.substr(0, outcome.exc_msg.find('\n'));
            }
        }
    }
    if (outcome.deadline_interrupted) {
        text += "\n[max_runtime hit: the deadline fired mid-run; the request was interrupted "
                "but still finished]";
    }
    if (!outcome.stdout_text.empty()) text += "\n--- stdout ---\n" + outcome.stdout_text;
    if (!outcome.process_output.empty()) {
        text += "\n--- process output (fd 1/2: subprocesses, os.system) ---\n" +
                outcome.process_output;
    }
    if (!outcome.stderr_text.empty()) text += "\n--- stderr ---\n" + outcome.stderr_text;
    if (!outcome.ok && !outcome.error.empty() && outcome.error != outcome.stderr_text) {
        text += "\n--- error ---\n" + outcome.error;
    }
    if (queued_ahead > 0) {
        text += "\n[ran after " + std::to_string(queued_ahead) +
                " earlier request(s) queued in this interpreter]";
    }
    if (static_cast<std::int64_t>(text.size()) > max_bytes) {
        const auto cap = static_cast<std::size_t>(max_bytes);
        const auto head = cap / 4;
        const auto tail = cap - head;
        // UTF-8-safe cuts: never split a multi-byte sequence at either end.
        std::string tail_text = text.substr(common::Utf8CeilToBoundary(text, text.size() - tail));
        text = common::TruncateUtf8Bytes(text, head) + "\n[... " +
               std::to_string(text.size() - cap) + " bytes omitted ...]\n" +
               tail_text;
    }
    return text;
}

}  // namespace minimal::tools_python
