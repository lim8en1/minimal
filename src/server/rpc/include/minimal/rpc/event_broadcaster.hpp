#pragma once

// The session's outbound FAN-OUT point: many producers, N draining consumers.
// Replaces the single-consumer EventQueue the session used to own (that class
// remains in the tree for other bounded-queue users) once sessions support more
// than one attached stream.
//
// THE INVARIANT THAT REPLACES "ONE CONSUMER BY DESIGN": the session's progress
// is never gated on any subscriber. The old queue BLOCKED its producer when a
// sole attached client stopped draining -- defensible when that client was the
// session's whole audience, unacceptable the moment a second client (or a
// detached period with nobody attached) can stall a turn it is not part of.
// Every queue here is therefore DROP-OLDEST: a full subscriber has its oldest
// event evicted, a resync latch is set, and a ResyncRequiredEvent is appended
// so the client knows to refetch the transcript. Publish never blocks.
//
// THE DURABLE LOG IS THE TRANSCRIPT IN SQLITE; events are ephemeral
// notifications. That is what makes dropping safe and what makes detach safe:
// a client that misses events recovers through GetMessages, a path it already
// has, not through event replay.
//
// THREE storage sites, one policy:
//
//   * per-subscriber queues -- one per attached stream, each bounded and
//     drop-oldest on its own. A slow client degrades alone.
//   * the backlog -- where events go while NO subscriber exists. Bounded and
//     drop-oldest exactly the same way; a drop latches the backlog. This is
//     the old pre-attach buffer's successor: a subagent's loop still starts
//     producing before its tab attaches, and a detached session's output
//     still lands somewhere bounded instead of stalling the worker.
//   * the resync latch -- per subscriber and on the backlog, latched by any
//     drop, surfaced as a ResyncRequiredEvent ("overflow"). The client's
//     recovery is refetch, so the latch is never cleared by draining.
//
// Subscribe decides between REPLAY and RESYNC. A session's FIRST subscriber
// seeds from the backlog -- byte-for-byte the old pre-attach semantics, which
// is what subagent attach depends on. Any LATER subscriber has provably missed
// events (everything delivered to the earlier subscriber), so it is told to
// resync ("late_attach") and then seeded with whatever the detached period
// left in the backlog. Either way the backlog is consumed: it exists for the
// zero-subscriber window only.
//
// Threading: producers (session worker, gRPC reader, watcher threads) call
// Publish from any thread; the gRPC handler calls Subscribe/Unsubscribe; each
// stream's writer thread calls Pop on its own handle. One set mutex guards the
// subscriber set and the backlog; each subscriber has its own mutex, and the
// lock order is set -> subscriber only, never the reverse.

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class EventBroadcaster {
public:
    // Opaque per-stream state. Held by shared_ptr so a writer thread can keep
    // popping (and drain a close) while the handler unsubscribes concurrently.
    struct SubscriberState;
    using Handle = std::shared_ptr<SubscriberState>;

    // The old EventQueue's bounds, unchanged: a ceiling that catches a runaway
    // producer, not a tuning knob (see event_queue.hpp for the arithmetic).
    static constexpr std::size_t kDefaultMaxEvents = 65536;
    static constexpr std::size_t kDefaultMaxBytes = 64ull << 20;

    // session_id is stamped onto the ResyncRequiredEvents this broadcaster
    // synthesizes -- they must be wire-valid ServerEvents like any other.
    explicit EventBroadcaster(std::string session_id,
                              std::size_t max_events = kDefaultMaxEvents,
                              std::size_t max_bytes = kDefaultMaxBytes);
    ~EventBroadcaster();

    EventBroadcaster(const EventBroadcaster&) = delete;
    EventBroadcaster& operator=(const EventBroadcaster&) = delete;

    // Attaches one stream. The FIRST subscriber receives the backlog (the
    // pre-attach buffer semantics subagent attach relies on); any later one
    // receives a ResyncRequiredEvent("late_attach") first -- it has
    // provably missed everything the earlier subscriber saw. Returns a handle
    // the stream's writer thread pops from.
    Handle Subscribe();

    // Detaches one stream. Idempotent; safe for a handle from an already-
    // closed broadcaster. The subscriber's queue is closed drain-first, so a
    // writer parked in Pop unwinds after delivering whatever it still holds.
    void Unsubscribe(const Handle& handle);

    // Broadcasts to every subscriber, or to the backlog when none exist.
    // Returns whether the event was accepted: false only when the broadcaster
    // is closed (the stream-sink abort contract the old Push carried -- the
    // loop treats a refusal as "stop the request"). Never blocks, never
    // refuses for capacity: a full destination drops its oldest and latches.
    bool Publish(v1::ServerEvent event);

    // Targets ONE subscriber -- attach-time metadata, prompt replay. Same
    // contract as Publish, plus false when the handle is unknown or closed.
    bool PublishTo(const Handle& handle, v1::ServerEvent event);

    // The writer thread's drain. Blocks until an event is available or this
    // subscriber's queue is closed AND drained (nullopt = no more, ever).
    std::optional<v1::ServerEvent> Pop(const Handle& handle);

    // Non-blocking variant, for tests and any consumer that polls.
    std::optional<v1::ServerEvent> TryPop(const Handle& handle);

    // Terminate's teardown: every subscriber queue closes drain-first (the
    // SessionEndedEvent published just before this still goes out), and the
    // broadcaster refuses further publishes. Idempotent.
    void CloseAll();

    bool closed() const;
    std::size_t subscriber_count() const;
    // Events waiting in the zero-subscriber backlog. Diagnostic; the count a
    // test needs when it wants "everything the session produced while nobody
    // was attached".
    std::size_t backlog_size() const;
    // Events waiting in one subscriber's queue. Diagnostic (tests, metrics);
    // not part of the producer contract -- Publish never reads it.
    std::size_t queued(const Handle& handle) const;

private:
    // Takes the subscriber's mutex itself. The *Locked helpers require the
    // caller to already hold it -- never nest them.
    bool Deliver(SubscriberState& sub, v1::ServerEvent event);
    void AcceptLocked(SubscriberState& sub, v1::ServerEvent event, std::size_t bytes);
    bool FitsLocked(const SubscriberState& sub, std::size_t bytes) const;
    // Latches the subscriber's resync flag (once) and appends the notice.
    void TakeResyncLocked(SubscriberState& sub, const char* reason);
    static std::size_t SizeOf(const v1::ServerEvent& event);

    const std::string session_id_;
    const std::size_t max_events_;
    const std::size_t max_bytes_;

    mutable std::mutex set_mutex_;
    std::vector<Handle> subscribers_;
    std::deque<v1::ServerEvent> backlog_;
    std::size_t backlog_bytes_ = 0;
    bool backlog_latched_ = false;
    // Set by the first Subscribe: from then on the backlog is a PARTIAL
    // record (everything the earlier subscriber took is gone), so later
    // subscribers always get a resync notice rather than a silent gap.
    bool ever_subscribed_ = false;
    bool closed_ = false;
};

}  // namespace minimal::rpc
