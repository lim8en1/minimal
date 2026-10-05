#include "minimal/rpc/event_broadcaster.hpp"

namespace minimal::rpc {

struct EventBroadcaster::SubscriberState {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedEvent> events;
    std::size_t bytes = 0;
    bool closed = false;
    // Latched by the first drop into this queue (or set at Subscribe for a
    // late attacher). Never cleared by draining: the client must refetch, and
    // one notice is enough -- a re-latched queue must not spam more.
    bool resync_latched = false;
};

std::size_t EventBroadcaster::SizeOf(const v1::ServerEvent& event) {
    return event.ByteSizeLong();
}

EventBroadcaster::EventBroadcaster(std::string session_id, std::size_t max_events,
                                   std::size_t max_bytes, std::size_t max_backlog_bytes)
    : session_id_(std::move(session_id)),
      max_events_(max_events),
      max_bytes_(max_bytes),
      max_backlog_bytes_(max_backlog_bytes) {}

EventBroadcaster::~EventBroadcaster() { CloseAll(); }

EventBroadcaster::Handle EventBroadcaster::Subscribe() {
    auto sub = std::make_shared<SubscriberState>();
    {
        std::lock_guard<std::mutex> lock(set_mutex_);
        if (closed_) {
            // A stream attaching to a session that is already tearing down:
            // hand back a queue that is closed-and-empty so its writer exits
            // immediately, the same graceful answer the old closed EventQueue
            // gave a late Pop. Returned HERE, before the shared tail below --
            // that tail unconditionally does `subscribers_.push_back(sub)`,
            // which would register a subscriber CloseAll() already returned
            // from and nothing will ever remove (no Unsubscribe is coming for
            // a stream that was never really attached), making
            // subscriber_count() lie during teardown.
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            sub->closed = true;
            return sub;
        } else if (ever_subscribed_) {
            // A LATER subscriber has provably missed events -- everything the
            // earlier subscriber's queue received. Resync first, then hand it
            // whatever the current zero-subscriber window accumulated (the
            // detached period's events; the earlier subscriber's share is
            // unrecoverable here and comes from the transcript instead).
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            TakeResyncLocked(*sub, "late_attach");
            for (auto& item : backlog_) AcceptLocked(*sub, std::move(item.event), item.bytes);
        } else {
            // FIRST subscriber: the pre-attach buffer's semantics, unchanged.
            // Unlatched backlog is a complete record of everything produced
            // so far; a latched one dropped events, so resync wins over
            // replaying a partial history the client would trust.
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            if (backlog_latched_) TakeResyncLocked(*sub, "overflow");
            for (auto& item : backlog_) AcceptLocked(*sub, std::move(item.event), item.bytes);
        }
        backlog_.clear();
        backlog_bytes_ = 0;
        backlog_latched_ = false;
        ever_subscribed_ = true;
        subscribers_.push_back(sub);
    }
    sub->cv.notify_one();
    return sub;
}

void EventBroadcaster::Unsubscribe(const Handle& handle) {
    if (handle == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(set_mutex_);
        std::erase_if(subscribers_,
                      [&handle](const Handle& h) { return h == handle; });
        std::lock_guard<std::mutex> sub_lock(handle->mutex);
        handle->closed = true;
    }
    handle->cv.notify_all();
}

bool EventBroadcaster::Publish(v1::ServerEvent event) {
    std::vector<Handle> targets;
    {
        std::lock_guard<std::mutex> lock(set_mutex_);
        if (closed_) return false;
        if (subscribers_.empty()) {
            // Nobody is listening: the backlog takes the event under the same
            // drop-oldest policy. A drop latches the backlog, so the next
            // subscriber resyncs instead of trusting a partial record.
            const std::size_t bytes = SizeOf(event);
            if (EvictOldestUntilFits(backlog_, backlog_bytes_, bytes, max_backlog_bytes_)) {
                backlog_latched_ = true;
            }
            backlog_bytes_ += bytes;
            backlog_.push_back(QueuedEvent{std::move(event), bytes});
            return true;
        }
        if (subscribers_.size() == 1) {
            // The common case: move, no copy.
            (void)Deliver(*subscribers_.front(), std::move(event));
            return true;
        }
        targets.assign(subscribers_.begin(), subscribers_.end());
    }
    for (auto& sub : targets) {
        // Each subscriber gets its own copy -- protobuf copy under the sub's
        // lock, never under the set lock (lock order: set -> sub only).
        (void)Deliver(*sub, event);
    }
    return true;
}

bool EventBroadcaster::PublishTo(const Handle& handle, v1::ServerEvent event) {
    if (handle == nullptr) return false;
    return Deliver(*handle, std::move(event));
}

bool EventBroadcaster::PublishToIfAbsent(const Handle& handle, v1::ServerEvent event) {
    if (handle == nullptr) return false;
    {
        // Only an event of the same oneof case can be identical, so the
        // integer compare runs first and only those few candidates (the open
        // prompts) are serialized. Serializing every queued event -- up to
        // the 65k / 64 MiB bound, once per replayed prompt -- was the cost.
        const auto wanted_case = event.payload_case();
        const std::string wanted = event.SerializeAsString();
        std::lock_guard<std::mutex> lock(handle->mutex);
        for (const auto& item : handle->events) {
            if (item.event.payload_case() != wanted_case) continue;
            if (item.event.SerializeAsString() == wanted) return true;  // already queued
        }
    }
    return Deliver(*handle, std::move(event));
}

std::optional<v1::ServerEvent> EventBroadcaster::Pop(const Handle& handle) {
    if (handle == nullptr) return std::nullopt;
    std::unique_lock<std::mutex> lock(handle->mutex);
    // Drain-then-close, the same contract the old queue's Pop carried: the
    // events already queued -- a terminate's SessionEndedEvent among them --
    // are the ones worth delivering before the stream goes away.
    handle->cv.wait(lock, [&] { return !handle->events.empty() || handle->closed; });
    if (handle->events.empty()) return std::nullopt;
    auto event = std::move(handle->events.front().event);
    handle->bytes -= handle->events.front().bytes;
    handle->events.pop_front();
    return event;
}

std::optional<v1::ServerEvent> EventBroadcaster::TryPop(const Handle& handle) {
    if (handle == nullptr) return std::nullopt;
    std::lock_guard<std::mutex> lock(handle->mutex);
    if (handle->events.empty()) return std::nullopt;
    auto event = std::move(handle->events.front().event);
    handle->bytes -= handle->events.front().bytes;
    handle->events.pop_front();
    return event;
}

void EventBroadcaster::CloseAll() {
    std::vector<Handle> targets;
    {
        std::lock_guard<std::mutex> lock(set_mutex_);
        if (closed_) return;
        closed_ = true;
        backlog_.clear();
        backlog_bytes_ = 0;
        targets = subscribers_;
        for (auto& sub : targets) {
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            sub->closed = true;
        }
    }
    for (auto& sub : targets) sub->cv.notify_all();
}

bool EventBroadcaster::closed() const {
    std::lock_guard<std::mutex> lock(set_mutex_);
    return closed_;
}

std::size_t EventBroadcaster::subscriber_count() const {
    std::lock_guard<std::mutex> lock(set_mutex_);
    return subscribers_.size();
}

std::size_t EventBroadcaster::backlog_size() const {
    std::lock_guard<std::mutex> lock(set_mutex_);
    return backlog_.size();
}

std::size_t EventBroadcaster::queued(const Handle& handle) const {
    if (handle == nullptr) return 0;
    std::lock_guard<std::mutex> lock(handle->mutex);
    return handle->events.size();
}

// Takes the SUBSCRIBER's mutex itself (the *Locked helpers below require the
// caller to hold it -- do not nest). Evicts oldest until the event fits -- a
// drop latches resync -- then accepts, then appends the one-time
// ResyncRequiredEvent AFTER the accepted event (everything before the notice
// may be incomplete; the notice itself and what follows are not).
// False means the subscriber was already closed and the event went nowhere.
bool EventBroadcaster::Deliver(SubscriberState& sub, v1::ServerEvent event) {
    bool dropped = false;
    {
        std::lock_guard<std::mutex> sub_lock(sub.mutex);
        if (sub.closed) return false;
        const std::size_t bytes = SizeOf(event);
        // A single event larger than the whole cap is accepted onto an EMPTY
        // queue rather than dropped into an unreachable loop (the old queue's
        // oversized-event rule, kept verbatim).
        dropped = EvictOldestUntilFits(sub.events, sub.bytes, bytes, max_bytes_);
        AcceptLocked(sub, std::move(event), bytes);
        if (dropped) TakeResyncLocked(sub, "overflow");
    }
    sub.cv.notify_one();
    return true;
}

void EventBroadcaster::AcceptLocked(SubscriberState& sub, v1::ServerEvent event,
                                    std::size_t bytes) {
    sub.bytes += bytes;
    sub.events.push_back(QueuedEvent{std::move(event), bytes});
}

bool EventBroadcaster::EvictOldestUntilFits(std::deque<QueuedEvent>& items,
                                            std::size_t& bytes_total, std::size_t incoming_bytes,
                                            std::size_t max_bytes) const {
    bool dropped = false;
    while (!items.empty() &&
           (items.size() >= max_events_ ||
            (max_bytes != 0 && bytes_total + incoming_bytes > max_bytes))) {
        bytes_total -= items.front().bytes;
        items.pop_front();
        dropped = true;
    }
    return dropped;
}

// Latches and appends the resync notice -- once. Re-latching (a second drop
// before the client reacts) must not queue a second notice.
void EventBroadcaster::TakeResyncLocked(SubscriberState& sub, const char* reason) {
    if (sub.resync_latched) return;
    sub.resync_latched = true;
    v1::ServerEvent event;
    event.set_session_id(session_id_);
    event.mutable_resync_required()->set_reason(reason);
    // Sized BEFORE the move: argument evaluation order is unspecified, and a
    // moved-from event sizes to 0 (the byte accounting would drift).
    const std::size_t bytes = SizeOf(event);
    AcceptLocked(sub, std::move(event), bytes);
}

}  // namespace minimal::rpc
