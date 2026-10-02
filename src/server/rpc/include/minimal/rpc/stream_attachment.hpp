#pragma once

// One client stream's attachment to a session, as a single owner.
//
// Attaching is three steps that must stay paired: subscribe to the session's
// broadcaster, tell the session a client arrived (Session::NotifyAttached keeps
// ever_attached()/has_subscribers() -- the reaper's initial-attach guard and
// detached-idle rule -- accurate), and, on the way out, unsubscribe and tell the
// session it left. They used to be spelled out at the top of OpenSession with an
// `attached` flag so a failed Subscribe never produced an unmatched
// NotifyDetached (which would leave has_subscribers() reading an underflowed
// non-zero and the session permanently unreapable). Here the pairing is the
// object's lifetime: construction either completes both halves or throws having
// done neither, and Detach() / destruction undoes exactly what was done, once.

#include <optional>

#include "minimal/agent/session.hpp"
#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/event_translator.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class StreamAttachment {
public:
    // The rpc transport a session was wired with (FinishSessionWiring). Checked:
    // a session wired with anything else is a programming error, not a cast to
    // trust. The one place the agent-side SessionTransport interface is
    // narrowed to the concrete type this layer built for it.
    static EventTranslator& TransportOf(agent::Session& session);

    // Subscribes and notifies the session. Throws (leaving nothing attached) if
    // either step fails.
    explicit StreamAttachment(agent::Session& session);
    ~StreamAttachment() { Detach(); }

    StreamAttachment(const StreamAttachment&) = delete;
    StreamAttachment& operator=(const StreamAttachment&) = delete;

    // This stream's subscriber handle (the broadcaster's per-stream queue).
    const EventBroadcaster::Handle& handle() const { return handle_; }

    // How many streams are subscribed to the session right now, this one
    // included.
    std::size_t SubscriberCount() const;

    // Targets THIS stream only: attach-time metadata and prompt replay. Never
    // blocks; false when the stream's queue is closed.
    bool PublishTo(v1::ServerEvent event) const;

    // Replays the prompts the worker is still parked on to this stream (an
    // earlier client crashed or detached before answering), skipping any the
    // stream's backlog already holds.
    void ReplayOpenPrompts() const;

    // The writer thread's drain: blocks until an event, or nullopt once the
    // stream's queue is closed and drained.
    std::optional<v1::ServerEvent> Pop() const;

    // Unsubscribes (closing the queue, which unwinds a writer parked in Pop) and
    // tells the session this client left. Idempotent.
    void Detach();

private:
    agent::Session& session_;
    EventTranslator& transport_;  // owned by the session's shared transport
    EventBroadcaster::Handle handle_;
    bool attached_ = false;
};

}  // namespace minimal::rpc
