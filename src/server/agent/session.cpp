#include "minimal/agent/session.hpp"

#include <exception>
#include <utility>

#include "minimal/rpc/proto_convert.hpp"

namespace minimal::agent {
namespace {

v1::ServerEvent MakeEvent(const std::string& session_id) {
    v1::ServerEvent event;
    event.set_session_id(session_id);
    return event;
}

}  // namespace

Session::Session(SessionParams params, AgentPreset preset,
                 std::unique_ptr<llm::LlmProvider> provider, std::unique_ptr<HistoryStore> store,
                 tools::ToolRegistry& registry, LoopConfig loop_config,
                 SessionObserverRegistry& observers, common::Logger& logger)
    : params_(std::move(params)),
      preset_(std::move(preset)),
      logger_(logger),
      observers_(observers),
      provider_(std::move(provider)),
      store_(std::move(store)),
      checker_(params_.session_id, queue_, pending_),
      // The run mode the client asked for wins over the configured default: it
      // is a property of the CLIENT, not of the config. A headless run has no
      // human to answer a prompt, so it must be able to say so. apply_run_mode()
      // is applied downstream, once, by ToolExecutor.
      executor_(registry, checker_, preset_.permission_mode, preset_.mcp_permissions,
                params_.run_mode_override.value_or(preset_.run_mode)) {
    info_.session_id = params_.session_id;
    info_.parent_session_id = params_.parent_session_id;
    info_.name = params_.name;
    info_.agent_preset = params_.agent_preset;
    info_.project_root = params_.project_root;

    SessionContext context{logger_};
    context.session_id = params_.session_id;
    context.project_root = params_.project_root;

    const std::string session_id = params_.session_id;
    auto* queue = &queue_;

    context.tool_output_sink = [queue, session_id](std::string_view tool_use_id,
                                                   std::string_view chunk) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* delta = event.mutable_tool_output_delta();
        delta->set_tool_use_id(std::string(tool_use_id));
        delta->set_chunk(std::string(chunk));
        queue->Push(std::move(event));
    };

    context.tool_lifecycle_sink = [queue, session_id](const ToolLifecycleEvent& lifecycle) {
        v1::ServerEvent event = MakeEvent(session_id);
        auto* execution = event.mutable_tool_execution();
        execution->set_tool_use_id(lifecycle.tool_use_id);
        execution->set_tool_name(lifecycle.tool_name);
        *execution->mutable_input() = rpc::JsonToStruct(lifecycle.input);
        execution->set_started(lifecycle.started);
        if (!lifecycle.started) execution->set_is_error(lifecycle.is_error);
        execution->set_started_at_unix_ms(lifecycle.started_at_unix_ms);
        queue->Push(std::move(event));
    };

    // Returning false aborts the provider request. This is the cancellation path
    // that termination reuses -- there is no second mechanism, and adding one is
    // how a session ends up with two half-working ways to stop.
    llm::StreamEventSink event_sink = [this, queue, session_id](const llm::StreamEvent& stream) {
        if (IsCancelled()) return false;
        v1::ServerEvent event = MakeEvent(session_id);
        *event.mutable_stream_event() = rpc::ToProto(stream);
        queue->Push(std::move(event));
        return true;
    };

    // The real summarizer. Passed as the plain function rather than wrapped,
    // because everything it needs -- provider, model, prompt, cancel flag -- is
    // an argument the loop supplies per call. That is what keeps a mid-session
    // /agent switch from compacting with the previous preset's prompt.
    loop_ = std::make_unique<AgentLoop>(*provider_, executor_, registry, *store_, preset_,
                                        loop_config, std::move(context),
                                        [this] { return IsCancelled(); }, std::move(event_sink),
                                        compact_messages);

    observers_.NotifySessionCreated(info_);
    worker_ = std::thread([this] { WorkerMain(); });
}

Session::~Session() { Terminate(); }

bool Session::IsCancelled() const {
    return cancel_.load(std::memory_order_acquire) || terminated_.load(std::memory_order_acquire);
}

void Session::SubmitTurn(std::string text) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (terminated_.load(std::memory_order_acquire)) return;
        turns_.push_back(std::move(text));
    }
    turn_cv_.notify_one();
}

void Session::Cancel() {
    cancel_.store(true, std::memory_order_release);
    // Wakes a worker parked on a permission prompt. Without this, cancelling
    // during an Ask would set a flag nobody is in a position to read.
    //
    // CancelAll, not Shutdown: this ends the TURN. The distinction is
    // load-bearing -- a permanent cancel here leaves every later permission
    // prompt in the session answering "no answer" instantly, which
    // WirePermissionChecker turns into Deny, so one CancelTurn would silently
    // deny-lock a session that reports itself alive and keeps accepting turns.
    pending_.CancelAll();
}

void Session::Terminate() {
    if (terminated_.exchange(true, std::memory_order_acq_rel)) {
        // Already terminating. Still join, so that a second caller does not
        // return while the worker is alive -- the destructor relies on it.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
        return;
    }

    cancel_.store(true, std::memory_order_release);
    // Shutdown, not CancelAll: the session is ending, so a worker that reaches a
    // gate mid-teardown must return rather than park on a reader that has
    // already stopped reading.
    pending_.Shutdown();
    turn_cv_.notify_all();

    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();

    // Closed only after the worker is joined, so every event the dying turn
    // produced is already queued. Close() drains before reporting closed, so the
    // client still receives them.
    queue_.Close();
    observers_.NotifySessionEnded(info_);
}

void Session::WorkerMain() {
    while (true) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            turn_cv_.wait(lock, [this] {
                return !turns_.empty() || terminated_.load(std::memory_order_acquire);
            });
            if (terminated_.load(std::memory_order_acquire)) return;
            text = std::move(turns_.front());
            turns_.pop_front();
            running_turn_ = true;
        }

        // A new turn starts uncancelled: Cancel() ends the turn in flight, not
        // the session. Terminate() is what ends the session, and it is checked
        // separately above.
        cancel_.store(false, std::memory_order_release);
        RunOneTurn(std::move(text));

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_turn_ = false;
        }
        turn_cv_.notify_all();
    }
}

void Session::RunOneTurn(std::string text) {
    const auto started = std::chrono::steady_clock::now();
    const std::int64_t first_seq = store_->last_appended_seq() + 1;

    TurnResult result;
    try {
        result = loop_->RunTurn(llm::Message{llm::Role::User, {llm::TextBlock{std::move(text)}}});
    } catch (const llm::LlmError& error) {
        // The loop rethrows once retries are exhausted. The session survives:
        // one failed turn is not a reason to tear down a conversation the user
        // can retry.
        v1::ServerEvent event = MakeEvent(params_.session_id);
        *event.mutable_error() = rpc::ToProto(error);
        queue_.Push(std::move(event));
        logger_.warn("session {}: turn failed: {}", params_.session_id, error.what());
    } catch (const std::exception& e) {
        v1::ServerEvent event = MakeEvent(params_.session_id);
        auto* error = event.mutable_error();
        error->set_category(v1::LLM_ERROR_CATEGORY_UNSPECIFIED);
        error->set_message(e.what());
        error->set_is_retryable(false);
        queue_.Push(std::move(event));
        logger_.warn("session {}: turn threw: {}", params_.session_id, e.what());
    }

    // BOTH endings run through here, and that is the point. A failed turn leaves
    // `result` default-constructed -- StopReason::Unknown, zero usage -- which is
    // exactly what a failure should report.
    //
    // The wire event is emitted on every exit path because a client that learns
    // a turn ended only by seeing it succeed hangs on every error. The observer
    // notification is the same obligation on the in-process side: an observer
    // holding per-turn state has no other signal that the turn is over, so
    // returning early from a catch block leaked it once per failure. Same
    // argument as emitting tool-finish even when the tool errored.
    EmitTurnCompleted(result.stop_reason, result.last_usage, result.hit_max_turns);

    TurnSummary summary;
    summary.first_seq = first_seq;
    summary.last_seq = store_->last_appended_seq();
    summary.stop_reason = result.stop_reason;
    summary.usage = result.last_usage;
    summary.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    observers_.NotifyTurnCompleted(info_, std::move(summary));
}

void Session::EmitTurnCompleted(llm::StopReason stop_reason, const llm::Usage& usage,
                                bool hit_max_turns) {
    v1::ServerEvent event = MakeEvent(params_.session_id);
    auto* completed = event.mutable_turn_completed();
    completed->set_stop_reason(rpc::ToProto(stop_reason));
    *completed->mutable_usage() = rpc::ToProto(usage);
    completed->set_hit_max_turns(hit_max_turns);
    queue_.Push(std::move(event));
}

bool Session::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return turns_.empty() && !running_turn_;
}

bool Session::WaitForIdleForTesting(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return turn_cv_.wait_for(lock, timeout, [this] {
        return (turns_.empty() && !running_turn_) || terminated_.load(std::memory_order_acquire);
    });
}

}  // namespace minimal::agent
