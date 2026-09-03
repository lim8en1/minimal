#pragma once

// A test-side subscriber to a Session's event broadcaster -- the stand-in for
// the OpenSession writer the e2e tier gets for free. Unit tests drive a
// Session directly, so they subscribe explicitly, drain explicitly, and
// unsubscribe deterministically.
//
// The FIRST subscription a session sees seeds from its zero-subscriber backlog
// (the pre-attach semantics subagent attach relies on); any LATER one is
// preceded by a ResyncRequiredEvent("late_attach") -- see event_broadcaster.hpp
// for the replay-vs-resync rule. Tests that assert on exact event sequences
// therefore keep ONE stream alive for the whole case rather than subscribing
// per drain.
//
// Session's public surface no longer names rpc::EventBroadcaster (Phase 4):
// this helper reaches the real transport through Session::transport(), the
// same opaque handle SessionServiceImpl casts back to rpc::EventTranslator.

#include <optional>
#include <vector>

#include "minimal/agent/session.hpp"
#include "minimal/rpc/event_translator.hpp"

namespace minimal::testing {

inline rpc::EventTranslator& Transport(agent::Session& session) {
    return *static_cast<rpc::EventTranslator*>(session.transport().get());
}

class SessionStream {
public:
    explicit SessionStream(agent::Session& session)
        : session_(session), handle_(Transport(session).queue().Subscribe()) {
        session_.NotifyAttached();
    }

    // OWNING variant: keeps the session alive at least as long as the stream,
    // so a session the registry erases mid-test cannot leave the destructor
    // unsubscribing through a dangling reference. `owned_` is declared before
    // `session_`, so it dies last.
    explicit SessionStream(const std::shared_ptr<agent::Session>& session)
        : owned_(session), session_(*session), handle_(Transport(*session).queue().Subscribe()) {
        session_.NotifyAttached();
    }

    ~SessionStream() {
        Transport(session_).queue().Unsubscribe(handle_);
        session_.NotifyDetached();
    }

    SessionStream(const SessionStream&) = delete;
    SessionStream& operator=(const SessionStream&) = delete;

    // Non-blocking drain of everything currently queued for this stream.
    std::vector<v1::ServerEvent> Drain() {
        std::vector<v1::ServerEvent> out;
        while (auto event = Transport(session_).queue().TryPop(handle_)) out.push_back(std::move(*event));
        return out;
    }

    // Blocking pop -- for drains that must wait on a queue a writer would
    // normally drain, e.g. until a Terminate closes it.
    std::optional<v1::ServerEvent> Pop() { return Transport(session_).queue().Pop(handle_); }

    std::size_t queued() const { return Transport(session_).queue().queued(handle_); }
    const rpc::EventBroadcaster::Handle& handle() const { return handle_; }

private:
    std::shared_ptr<agent::Session> owned_;
    agent::Session& session_;
    rpc::EventBroadcaster::Handle handle_;
};

// One-shot drain: subscribe, drain, unsubscribe. Convenience for cases that
// inspect events at most once per subscription window. CAUTION: an event
// delivered to an EARLIER subscriber's queue is gone from here -- a session
// with a long-lived subscriber shows this helper only a resync notice plus
// the zero-subscriber backlog. Keep one persistent SessionStream instead
// when drains must see a complete, ordered history.
inline std::vector<v1::ServerEvent> DrainEvents(agent::Session& session) {
    SessionStream stream(session);
    return stream.Drain();
}

}  // namespace minimal::testing
