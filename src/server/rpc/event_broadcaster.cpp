#include "minimal/rpc/event_broadcaster.hpp"

namespace minimal::rpc {

struct EventBroadcaster::SubscriberState {
    struct Item {
        v1::ServerEvent event;
        std::size_t bytes = 0;
    };

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> events;
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
                                   std::size_t max_bytes)
    : session_id_(std::move(session_id)), max_events_(max_events), max_bytes_(max_bytes) {}

EventBroadcaster::~EventBroadcaster() { CloseAll(); }

EventBroadcaster::Handle EventBroadcaster::Subscribe() {
    auto sub = std::make_shared<SubscriberState>();
    {
        std::lock_guard<std::mutex> lock(set_mutex_);
        if (closed_) {
            // A stream attaching to a session that is already tearing down:
            // hand back a queue that is closed-and-empty so its writer exits
            // immediately, the same graceful answer the old closed EventQueue
            // gave a late Pop.
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            sub->closed = true;
        } else if (ever_subscribed_) {
            // A LATER subscriber has provably missed events -- everything the
            // earlier subscriber's queue received. Resync first, then hand it
            // whatever the current zero-subscriber window accumulated (the
            // detached period's events; the earlier subscriber's share is
            // unrecoverable here and comes from the transcript instead).
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            TakeResyncLocked(*sub, "late_attach");
            for (auto& event : backlog_) {
                AcceptLocked(*sub, std::move(event), SizeOf(event));
            }
        } else {
            // FIRST subscriber: the pre-attach buffer's semantics, unchanged.
            // Unlatched backlog is a complete record of everything produced
            // so far; a latched one dropped events, so resync wins over
            // replaying a partial history the client would trust.
            std::lock_guard<std::mutex> sub_lock(sub->mutex);
            if (backlog_latched_) TakeResyncLocked(*sub, "overflow");
            for (auto& event : backlog_) {
                AcceptLocked(*sub, std::move(event), SizeOf(event));
            }
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
            while (!backlog_.empty() &&
                   (backlog_.size() >= max_events_ ||
                    (max_bytes_ != 0 && backlog_bytes_ + bytes > max_bytes_))) {
                backlog_bytes_ -= SizeOf(backlog_.front());
                backlog_.pop_front();
                backlog_latched_ = true;
            }
            backlog_bytes_ += bytes;
            backlog_.push_back(std::move(event));
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
        while (!FitsLocked(sub, bytes) && !sub.events.empty()) {
            sub.bytes -= sub.events.front().bytes;
            sub.events.pop_front();
            dropped = true;
        }
        AcceptLocked(sub, std::move(event), bytes);
        if (dropped) TakeResyncLocked(sub, "overflow");
    }
    sub.cv.notify_one();
    return true;
}

void EventBroadcaster::AcceptLocked(SubscriberState& sub, v1::ServerEvent event,
                                    std::size_t bytes) {
    sub.bytes += bytes;
    sub.events.push_back(SubscriberState::Item{std::move(event), bytes});
}

bool EventBroadcaster::FitsLocked(const SubscriberState& sub, std::size_t bytes) const {
    if (sub.events.size() >= max_events_) return false;
    if (max_bytes_ == 0) return true;
    return sub.events.empty() || sub.bytes + bytes <= max_bytes_;
}

// Latches and appends the resync notice -- once. Re-latching (a second drop
// before the client reacts) must not queue a second notice.
void EventBroadcaster::TakeResyncLocked(SubscriberState& sub, const char* reason) {
    if (sub.resync_latched) return;
    sub.resync_latched = true;
    v1::ServerEvent event;
    event.set_session_id(session_id_);
    event.mutable_resync_required()->set_reason(reason);
    AcceptLocked(sub, std::move(event), SizeOf(event));
}

}  // namespace minimal::rpc
