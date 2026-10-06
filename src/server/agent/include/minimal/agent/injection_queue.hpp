#pragma once

// Cross-thread staging for messages that enter the conversation OUT OF BAND --
// a non-blocking subagent's completion, a background bash task's result -- and
// therefore cannot be appended whenever they finish: landing a message between
// a tool_use and its tool_result is a hard 400 on both wire formats. The
// session worker drains the queue at TURN BOUNDARIES (AgentLoop flushes at the
// start of every turn and after each tool cycle).
//
// WHY IT LIVES IN SESSIONCHANNELS AND NOT ON AGENTLOOP: the producers are
// foreign threads (the child's worker, a background task's watcher), and the
// handles they capture must stay safe past the session's death. The queue is
// owned by SessionChannels -- the shared_ptr block that already travels
// wherever a cross-thread handle needs one -- so a captured shared_ptr keeps
// the (harmless, empty) queue alive after the session is gone and Push simply
// reports refusal after Close. A tool holding `ctx.schedule_injection` across
// an invocation therefore cannot dangle, and it needs no knowledge of Session
// or AgentLoop: neither type is plugin surface.
//
// THREAD CONTRACT: Push is callable from ANY thread. DrainAll, Clear and empty
// are session-worker-only (DrainAll via the agent loop's flush; Clear via
// /clear; empty via Session's wake predicate, which holds the session mutex).
// Close is called by Session::Terminate.

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

#include "minimal/llm/message.hpp"
#include "minimal/tools/permission_model.hpp"
#include "minimal/agent/policy.hpp"

namespace minimal::agent {

inline bool IsUserTextMessage(const llm::Message& message) {
    if (message.role != llm::Role::User) return false;
    for (const auto& block : message.content) {
        if (const auto* text = std::get_if<llm::TextBlock>(&block);
            text != nullptr && !text->text.empty()) {
            // A task-result notification is machine-written, not a prompt.
            if (text->text.starts_with(llm::kTaskResultTag)) return false;
            return true;
        }
    }
    return false;
}

// A permission check a message must pass when the worker drains it
// (ToolExecutor::CheckSubject with `owner`'s model) -- how an external
// signal is gated per receiving session without blocking its producer.
struct InjectionGate {
    std::string owner;
    tools::PermissionSubject subject;
    std::string summary;
};

class InjectionQueue {
public:
    // The staging bound. The work queue and the outbound event queue are
    // both documented bounded; this third queue silently was not -- producers
    // are non-blocking subagent completions and background task results (up to
    // 1 MB each) landing on an IDLE parent with wake_on_injection off, which
    // flushes only at the next turn. Over the limit is a REFUSAL, never a
    // queue: the caller logs the loss, the same contract Close has. Bounded by
    // message count (kMaxPendingInjections).

    // False means the message was NOT staged -- the owning session has
    // terminated (Close ran) or the staging bound is hit. The caller logs the
    // loss, the same contract Session::QueueInjectedMessage has always had.
    bool Push(llm::Message message, std::optional<InjectionGate> gate = std::nullopt) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            if (pending_.size() >= kMaxPendingInjections) return false;
            pending_.push_back(Staged{std::move(message), std::move(gate)});
        }
        return true;
    }

    // Session worker. Drains everything staged so far, in arrival order,
    // through `sink`. The swap happens under the lock; the sink runs outside
    // it -- AppendAndPersist touches SQLite and must not hold this mutex.
    // A sink taking (llm::Message, const InjectionGate*) also sees each
    // message's gate (null: ungated); a one-argument sink sees messages only.
    template <typename Sink>
    void DrainAll(Sink&& sink) {
        std::deque<Staged> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready.swap(pending_);
        }
        for (auto& staged : ready) {
            if constexpr (std::is_invocable_v<Sink&, llm::Message, const InjectionGate*>) {
                sink(std::move(staged.message), staged.gate ? &*staged.gate : nullptr);
            } else {
                sink(std::move(staged.message));
            }
        }
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.empty();
    }

    // True when the queue holds at least one USER-TEXT message -- i.e. a
    // real-time steering submission (Session routes a mid-turn UserChatTurn
    // here), as opposed to the tool-result-only shapes subagent completions
    // and background task results arrive as. The session worker's wait
    // predicate reads this: a steering message that missed the turn's flush
    // (the model had already finished, or the turn ended paused) must be
    // ANSWERED, so the worker wakes and runs a seedless continuation turn
    // regardless of the wake_on_injection setting -- a user message is
    // explicit intent, which a background result is not.
    bool HasUserText() const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& staged : pending_) {
            if (IsUserTextMessage(staged.message)) return true;
        }
        return false;
    }

    // /clear drops the VIEW of the conversation, and a staged completion is
    // part of that view's future: draining the history without this would
    // replay a subagent result the user just wiped.
    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.clear();
    }

    // Session teardown. Push refuses afterwards. Already-staged messages are
    // still drainable (and will be, by nothing -- the worker is exiting; the
    // messages die with the queue, which is correct for a session nobody is
    // attached to).
    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

private:
    struct Staged {
        llm::Message message;
        std::optional<InjectionGate> gate;
    };
    mutable std::mutex mutex_;
    std::deque<Staged> pending_;
    bool closed_ = false;
};

}  // namespace minimal::agent
