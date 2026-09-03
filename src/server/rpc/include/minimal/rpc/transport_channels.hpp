#pragma once

// The wire-facing half of what SessionChannels used to bundle whole
// (agent/session.hpp's SessionChannels keeps the domain half: InjectionQueue
// and background_tasks_live). Split so agent::Session never needs to name
// EventBroadcaster or PendingRequests -- see EventTranslator, the one thing
// built against this block that agent/ code ever touches, and only through
// the opaque agent::SessionEventSink / Session::transport() handle.
//
// Same shared_ptr-by-value lifetime pattern SessionChannels used: a sink or
// checker that must outlive the worker thread (or Session itself) captures
// this block, not `this` or a raw reference, so Publish/Resolve into a torn-
// down session refuses harmlessly instead of dangling.

#include <memory>
#include <string>
#include <utility>

#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"

namespace minimal::rpc {

struct TransportChannels {
    // session_id is threaded to EventBroadcaster because the resync notices it
    // synthesizes must be wire-valid ServerEvents (see event_broadcaster.hpp).
    explicit TransportChannels(std::string session_id) : queue(std::move(session_id)) {}

    EventBroadcaster queue;
    PendingRequests pending;
};

inline std::shared_ptr<TransportChannels> MakeTransportChannels(std::string session_id) {
    return std::make_shared<TransportChannels>(std::move(session_id));
}

}  // namespace minimal::rpc
