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

#include "driver_source.hpp"

namespace minimal::tools_python {
namespace {

using tools::BackgroundTaskEvent;

// The reader's slice. Long enough that an idle interpreter costs almost
// nothing; short enough that session-death expiry and an outside HardKill are
// felt within a fraction of a second.
constexpr int kPollSliceMs = 100;
// A protocol line above this is corruption (the driver caps its own captures
// far below); treated as death -- degradation, never a wedge.
constexpr std::size_t kMaxLineBytes = 64ULL * 1024 * 1024;
constexpr std::size_t kMaxDiagBytes = 64 * 1024;

// Sync waiters poll the cancel flag on this slice while parked on the
// interpreter cv (two cvs -- the token's and the interpreter's -- cannot be
// waited on together; a slice bounds cancel latency without a second
// registration mechanism).
constexpr auto kCancelSlice = std::chrono::milliseconds(50);

// Poll timeout bounded by the nearest armed request deadline, clamped to the
// slice: deadline enforcement never waits longer than one slice past expiry.
int BoundedSlice(int slice_ms, std::chrono::steady_clock::duration until) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(until).count();
    if (ms <= 0) return 0;
    return static_cast<int>(std::min<std::int64_t>(slice_ms, ms));
}

// nlohmann's value() throws type_error when the key is present with the
// wrong type -- fine for a JSON payload we authored ourselves, wrong for a
// protocol line whose author is the driver process, which a corrupted or
// hostile child (executed code writing to fd 3 -- see driver_source.hpp) can
// shape however it likes. These never throw: a present-but-wrong-typed field
// reads as absent, same as a missing one.
std::string StringField(const nlohmann::json& body, const char* key,
                        std::string fallback = {}) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

bool BoolField(const nlohmann::json& body, const char* key, bool fallback = false) {
    const auto it = body.find(key);
    if (it == body.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

}  // namespace

Interpreter::Interpreter(LaunchedInterpreter proc, InterpreterConfig config,
                         const PythonSettings& settings, common::Logger& logger)
    : config_(std::move(config)),
      settings_(settings),
      logger_(logger),
      pid_(proc.pid),
      in_fd_(proc.in_fd),
      out_fd_(proc.out_fd),
      diag_fd_(proc.diag_fd),
      stream_fd_(proc.stream_fd),
      user_in_fd_(proc.user_in_fd),
      declared_(config_.declared) {
    // python_input never parks the session worker: a full pipe (the script is
    // not consuming) must surface as a partial write, not a blocked write.
    if (user_in_fd_.get() >= 0) {
        const int flags = ::fcntl(user_in_fd_.get(), F_GETFL, 0);
        if (flags >= 0) ::fcntl(user_in_fd_.get(), F_SETFL, flags | O_NONBLOCK);
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

    // The interpreter owns the reader thread JOINABLY: the destructor kills
    // the process first (EOF wakes the reader within one poll slice) and joins
    // before any member dies, so the reader can safely touch members up to the
    // moment join() returns.
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
        // under (ready_ at DrainLine's "ready" branch, diag_tail_ on every
        // diagnostics chunk): the reader is still running here regardless of
        // which way the wait ended, so an unlocked read races it.
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

void Interpreter::ReaderLoop() {
    std::string line;
    bool out_open = true, diag_open = true, stream_open = true;
    while (out_open || diag_open || stream_open) {
        if (dead_.load(std::memory_order_acquire)) return;  // HardKill cleaned up already
        if (config_.session_lifetime.expired()) {
            // Session terminate: the kill path. There is no synchronous plugin
            // kill hook at teardown (OnSessionEnded is fire-and-forget) -- this
            // expiry poll IS the mechanism, at one poll slice of lag.
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
        int timeout = kPollSliceMs;
        std::shared_ptr<Pending> fire, kill;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (auto p = RunningPendingLocked()) {
                StampRunningLocked(p, now);
                if (p->deadline) {
                    if (!p->deadline_fired && now >= *p->deadline) {
                        p->deadline_fired = true;
                        p->deadline_fired_at = now;
                        fire = p;
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
            HardKill(("request " + kill->id + " exceeded its max_runtime deadline and did not " +
                      "react to the interrupt within " + std::to_string(settings_.grace_seconds) +
                      "s; the interpreter was killed (state lost) to stop it")
                         .c_str());
            return;
        }
        if (fire) {
            if (fire->handles.emit_task_event) {
                try {
                    BackgroundTaskEvent event;
                    event.task_id = fire->id;
                    event.tool_use_id = fire->handles.tool_use_id;
                    event.command = fire->handles.label;
                    event.pid = pid_;
                    event.state = BackgroundTaskEvent::State::kTimedOut;
                    fire->handles.emit_task_event(event);
                } catch (...) {
                }
            }
            SoftInterrupt();
        }

        pollfd fds[3];
        int nfds = 0;
        const int out_slot = out_open ? nfds++ : -1;
        if (out_open) fds[out_slot] = pollfd{out_fd_.get(), POLLIN, 0};
        const int diag_slot = diag_open ? nfds++ : -1;
        if (diag_open) fds[diag_slot] = pollfd{diag_fd_.get(), POLLIN, 0};
        const int stream_slot = stream_open ? nfds++ : -1;
        if (stream_open) fds[stream_slot] = pollfd{stream_fd_.get(), POLLIN, 0};

        const int rc = poll(fds, static_cast<nfds_t>(nfds), timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            MarkDead("interpreter pipe error", false);
            return;
        }

        char chunk[8192];
        if (stream_slot >= 0 && (fds[stream_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(stream_fd_.get(), chunk, sizeof(chunk));
            if (n > 0) {
                // Attribute to the running request (FIFO: at most one). An
                // idle interpreter producing stream bytes is corruption from
                // a previous life -- drained and dropped.
                const std::lock_guard<std::mutex> lock(mu_);
                if (auto p = RunningPendingLocked(); p && !p->done) {
                    p->ring.Append(std::string_view(chunk, static_cast<std::size_t>(n)));
                    if (p->handles.emit_output) {
                        try {
                            p->handles.emit_output(std::string_view(chunk, static_cast<std::size_t>(n)));
                        } catch (...) {
                        }
                    }
                }
            } else if (n == 0 || errno != EINTR) {
                stream_open = false;
            }
        }
        if (out_slot >= 0 && (fds[out_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(out_fd_.get(), chunk, sizeof(chunk));
            if (n > 0) {
                line.append(chunk, static_cast<std::size_t>(n));
                std::size_t start = 0;
                while (true) {
                    const auto pos = line.find('\n', start);
                    if (pos == std::string::npos) break;
                    // DrainLine reads through StringField/BoolField, which
                    // never throw -- this guard is a second line of defense,
                    // not the mechanism: the reader thread has nowhere to
                    // send an exception (it runs detached from any caller's
                    // stack), so an uncaught throw here would std::terminate
                    // the whole server over one malformed protocol line.
                    try {
                        DrainLine(std::string_view(line).substr(start, pos - start));
                    } catch (const std::exception& error) {
                        logger_.warn("python[{}]: DrainLine threw: {}", pid_, error.what());
                        MarkDead("protocol corruption", config_.session_lifetime.expired());
                        return;
                    }
                    if (dead_.load(std::memory_order_acquire)) return;
                    start = pos + 1;
                }
                line.erase(0, start);
                if (line.size() > kMaxLineBytes) {
                    MarkDead("interpreter produced an oversized protocol line", false);
                    return;
                }
            } else if (n == 0 || errno != EINTR) {
                // EOF: the process exited on its own (crash, os._exit, kill).
                MarkDead("interpreter exited" +
                             (diag_tail_.empty() ? "" : ": " + diag_tail_),
                         config_.session_lifetime.expired());
                return;
            }
        }
        if (diag_slot >= 0 && (fds[diag_slot].revents & (POLLIN | POLLHUP | POLLERR))) {
            const ssize_t n = read(diag_fd_.get(), chunk, sizeof(chunk));
            if (n > 0) {
                // Locked: Spawn's startup wait (and any future cross-thread
                // reader) reads diag_tail_ under mu_, so the append must be
                // under it too -- this was previously unsynchronized.
                std::lock_guard<std::mutex> lock(mu_);
                diag_tail_.append(chunk, static_cast<std::size_t>(n));
                if (diag_tail_.size() > kMaxDiagBytes) {
                    diag_tail_.erase(0, diag_tail_.size() - kMaxDiagBytes);
                }
            } else if (n == 0 || errno != EINTR) {
                diag_open = false;  // diagnostics closed; the protocol side keeps this loop alive
            }
        }
    }
}

void Interpreter::DrainLine(std::string_view line) {
    if (line.empty()) return;
    nlohmann::json body = nlohmann::json::parse(line, nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        logger_.warn("python[{}]: undecodable protocol line ({} bytes)", pid_, line.size());
        MarkDead("protocol corruption", config_.session_lifetime.expired());
        return;
    }
    if (body.contains("ready")) {
        std::lock_guard<std::mutex> lock(mu_);
        version_ = StringField(body, "version", "unknown");
        ready_ = true;
        cv_.notify_all();
        return;
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
        return;
    }
    if (body.contains("bad_request")) {
        logger_.warn("python[{}]: driver rejected a request: {}", pid_,
                     StringField(body, "bad_request"));
        return;
    }
    const auto id_it = body.find("id");
    if (id_it == body.end() || !id_it->is_string()) {
        logger_.warn("python[{}]: protocol line without an id; ignoring", pid_);
        return;
    }
    RequestOutcome outcome;
    outcome.ok = BoolField(body, "ok");
    // stdout rides BOTH response kinds: a request that ran and then failed
    // keeps everything it printed before the exception.
    outcome.stdout_text = StringField(body, "stdout");
    outcome.stderr_text = StringField(body, "stderr");
    if (outcome.ok) {
        outcome.result = StringField(body, "result");
        outcome.result_json = StringField(body, "result_json");
    } else {
        outcome.error = StringField(body, "error", "request failed");
        outcome.exc_type = StringField(body, "exc");
        outcome.exc_msg = StringField(body, "exc_msg");
    }
    DeliverResponse(id_it->get<std::string>(), std::move(outcome));
}

void Interpreter::DeliverResponse(const std::string& id, RequestOutcome outcome) {
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = pending_.find(id);
        if (it == pending_.end()) {
            logger_.warn("python[{}]: response for unknown request {}", pid_, id);
            return;
        }
        p = it->second;
        p->outcome = std::move(outcome);
        // The deadline flag rides the outcome: a result that landed despite
        // the SIGINT is real, but the model must know the deadline was felt.
        p->outcome.deadline_interrupted = p->deadline_fired;
        p->done = true;
        ++progress_;
        cv_.notify_all();
        // A SYNC request's result is CLAIMED by its WaitSync (which erases the
        // entry): the response can beat the waiter to the map, and erasing
        // here would lose it -- the tool call would report a dead interpreter
        // for a request that completed. Async entries are consumed here and
        // now (the reader is their only consumer after the injection).
        if (p->async) pending_.erase(it);
        // The completion record (any delivery path): python_output's window
        // onto the completion->injection gap. Bounded metadata ring.
        const auto now = std::chrono::steady_clock::now();
        const double seconds =
            p->running_since ? std::chrono::duration<double>(now - *p->running_since).count() : 0.0;
        finished_.push_back(FinishedEntry{p->id, p->outcome, seconds});
        if (finished_.size() > kFinishedRing) finished_.pop_front();
    }

    const bool session_dead = p->handles.session_lifetime.expired();
    if (p->handles.emit_task_event) {
        try {
            BackgroundTaskEvent event;
            event.task_id = p->id;
            event.tool_use_id = p->handles.tool_use_id;
            event.command = p->handles.label;
            event.pid = pid_;
            event.state = p->outcome.ok ? BackgroundTaskEvent::State::kCompleted
                                        : BackgroundTaskEvent::State::kFailed;
            p->handles.emit_task_event(event);
        } catch (...) {
        }
    }
    // The lifecycle event went out first, so a client marks the row terminal
    // before (or at worst alongside) the injected result arrives. A session
    // that is gone refuses the injection anyway -- skipping is equivalent.
    if (p->async && !session_dead) InjectAsync(*p);
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
                                               std::int64_t max_runtime_seconds) {
    if (dead_.load(std::memory_order_acquire)) {
        *error = "interpreter is not running";
        return std::nullopt;
    }
    static std::atomic<std::uint64_t> next_id{1};
    const auto id = "py-" + std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
    auto payload = nlohmann::json{{"id", id}, {"code", code}}.dump();
    payload.push_back('\n');

    {
        std::lock_guard<std::mutex> lock(mu_);
        auto p = std::make_shared<Pending>(static_cast<std::size_t>(settings_.stream_head_bytes),
                                           static_cast<std::size_t>(settings_.stream_tail_bytes));
        p->id = id;
        p->seq = next_seq_++;
        p->async = async;
        p->handles = std::move(handles);
        p->max_runtime_seconds = max_runtime_seconds > 0 ? max_runtime_seconds : 0;
        pending_.emplace(id, std::move(p));
    }

    // One writer at a time: both tools are parallel_safe() == false, so the
    // loop runs them one at a time on the worker -- the serialized FIFO queue
    // is the semantics. A line larger than the pipe buffer blocks here until
    // the driver drains, which is exactly "queued behind the running request".
    std::size_t written = 0;
    while (written < payload.size()) {
        const ssize_t n =
            ::write(in_fd_.get(), payload.data() + written, payload.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            {
                std::lock_guard<std::mutex> lock(mu_);
                pending_.erase(id);
            }
            *error = std::string("failed to write to the interpreter: ") + std::strerror(errno);
            return std::nullopt;
        }
        written += static_cast<std::size_t>(n);
    }
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
        if (now >= deadline) {
            // THE DEMOTION: the deadline converts the request to async
            // delivery -- the interpreter and its state are untouched, and the
            // reader will inject the result at a turn boundary when it
            // eventually completes. Under mu_, so a response arriving in the
            // race window is either seen as done above or delivered async.
            p->async = true;
            return WaitOutcome::kDemoted;
        }
        if (cancel && cancel->IsCancelled()) {
            // CancelTurn never kills background work; identical to the
            // deadline case, only the wording differs.
            p->async = true;
            return WaitOutcome::kCancelled;
        }
        cv_.wait_until(lock, std::min(deadline, now + kCancelSlice));
    }
}

void Interpreter::SoftInterrupt() { killpg(pid_, SIGINT); }

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
        p->deadline = now + std::chrono::seconds(p->max_runtime_seconds);
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

Interpreter::InterruptOutcome Interpreter::WaitForInterrupt(
    std::chrono::milliseconds grace, const tools::CancelTokenPtr& cancel) {
    std::unique_lock<std::mutex> lock(mu_);
    const std::uint64_t start = progress_;
    const std::uint64_t start_idle = idle_interrupts_;
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (true) {
        if (progress_ != start) {
            return idle_interrupts_ != start_idle ? InterruptOutcome::kIdleInterrupt
                                                   : InterruptOutcome::kResponse;
        }
        if (dead_.load(std::memory_order_acquire)) return InterruptOutcome::kNoReaction;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return InterruptOutcome::kNoReaction;
        if (cancel && cancel->IsCancelled()) return InterruptOutcome::kNoReaction;
        cv_.wait_until(lock, std::min(deadline, now + kCancelSlice));
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
    // `reason` rides into every pending outcome below; there is no separate
    // death record to keep.

    std::vector<std::shared_ptr<Pending>> to_fail;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [id, p] : pending_) {
            // Filled UNDER mu_: sync waiters read these fields under it.
            p->outcome = RequestOutcome{};
            p->outcome.error = reason;
            p->done = true;
            to_fail.push_back(p);
        }
        pending_.clear();
        ++progress_;
        cv_.notify_all();
    }
    for (const auto& p : to_fail) {
        if (session_dead) continue;  // the session is gone; nothing to report to
        // Only ASYNC requests are injected: their completion would otherwise
        // never be delivered. A SYNC request's tool call is the delivery --
        // WaitSync wakes on the filled outcome above (or returns kDead when
        // the entry is already gone) and the tool reports the loss itself;
        // injecting too would duplicate the tool_use_id's result, which the
        // provider rejects as an unexpected tool_result.
        if (!p->async) continue;
        // The session is ALIVE and this death is an explicit kill (or a
        // crash): report each lost request honestly.
        if (p->handles.emit_task_event) {
            try {
                BackgroundTaskEvent event;
                event.task_id = p->id;
                event.tool_use_id = p->handles.tool_use_id;
                event.command = p->handles.label;
                event.pid = pid_;
                event.state = BackgroundTaskEvent::State::kFailed;
                p->handles.emit_task_event(event);
            } catch (...) {
            }
        }
        InjectAsync(*p);
    }
}

void Interpreter::HardKill(const char* reason) {
    MarkDead(reason, config_.session_lifetime.expired());
    // ReapAfterKill includes the group kill (plus the fork-to-setsid window
    // via the plain kill). An unreaped child (D state) is logged, never
    // waited on forever.
    if (!ReapAfterKill(pid_)) {
        logger_.warn("python[{}]: child not reaped within the post-kill budget ({})",
                     pid_, reason);
    }
    ForgetPgid(pid_);
    // The reader exits on its next slice (dead_ is set). It holds NO
    // shared_ptr -- it captures raw `this` (StartReader) -- so it is safe
    // only because ~Interpreter joins it before any member is torn down
    // (see the class comment's THREAD/OWNERSHIP CONTRACT). Do not drop that
    // join.
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
                          std::size_t queued_behind, std::int64_t max_bytes) {
    std::string text;
    if (outcome.ok) {
        // JSON-preferred: a serializable trailing expression is displayed as
        // its JSON rendering; the repr stays the fallback for rich objects.
        const std::string& value =
            outcome.result_json.empty() ? outcome.result : outcome.result_json;
        text = id + (value.empty() ? " ok" : " ok: " + value);
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
    if (!outcome.stderr_text.empty()) text += "\n--- stderr ---\n" + outcome.stderr_text;
    if (!outcome.ok && !outcome.error.empty() && outcome.error != outcome.stderr_text) {
        text += "\n--- error ---\n" + outcome.error;
    }
    if (queued_behind > 0) {
        text += "\n[queued behind " + std::to_string(queued_behind) +
                " other request(s) in this interpreter]";
    }
    if (static_cast<std::int64_t>(text.size()) > max_bytes) {
        const auto cap = static_cast<std::size_t>(max_bytes);
        const auto head = cap / 4;
        const auto tail = cap - head;
        text = text.substr(0, head) + "\n[... " + std::to_string(text.size() - cap) +
               " bytes omitted ...]\n" + text.substr(text.size() - tail);
    }
    return text;
}

}  // namespace minimal::tools_python
