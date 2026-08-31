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
#include <utility>

#include "minimal/llm/message.hpp"

namespace minimal::agent {

class InjectionQueue {
public:
    // False means the owning session has terminated (Close ran) and the
    // message was NOT staged -- the caller logs the loss, the same contract
    // Session::QueueInjectedMessage has always had.
    bool Push(llm::Message message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (closed_) return false;
            pending_.push_back(std::move(message));
        }
        return true;
    }

    // Session worker. Drains everything staged so far, in arrival order,
    // through `sink`. The swap happens under the lock; the sink runs outside
    // it -- AppendAndPersist touches SQLite and must not hold this mutex.
    template <typename Sink>
    void DrainAll(Sink&& sink) {
        std::deque<llm::Message> ready;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ready.swap(pending_);
        }
        for (auto& message : ready) sink(std::move(message));
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.empty();
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
    mutable std::mutex mutex_;
    std::deque<llm::Message> pending_;
    bool closed_ = false;
};

}  // namespace minimal::agent
