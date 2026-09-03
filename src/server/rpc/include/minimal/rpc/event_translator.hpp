#pragma once

// Implements agent::SessionEventSink against a real TransportChannels. This is
// THE seam that replaces agent::Session's direct v1::ServerEvent construction:
// Session builds an agent::SessionEvent, hands it to a sink, and everything
// wire-shaped happens here, one level removed.
//
// Deliberately a small, copyable value -- {session_id, shared_ptr<TransportChannels>}
// -- not a class Session holds by reference. Every copy independently keeps
// TransportChannels (hence the real EventBroadcaster) alive, which is what
// lets a sink captured by a detached watcher thread outlive both the worker
// and Session itself: the same shared_ptr-by-value lifetime pattern
// SessionChannels used before the split (session.cpp's channels_ capture),
// just retargeted at the transport half.

#include <string>
#include <utility>

#include "minimal/agent/session_event.hpp"
#include "minimal/rpc/proto_convert.hpp"
#include "minimal/rpc/transport_channels.hpp"

namespace minimal::rpc {

class EventTranslator {
public:
    EventTranslator(std::string session_id, std::shared_ptr<TransportChannels> transport)
        : session_id_(std::move(session_id)), transport_(std::move(transport)) {}

    // Usable directly as an agent::SessionEventSink.
    bool operator()(const agent::SessionEvent& event) const { return Publish(event); }

    bool Publish(const agent::SessionEvent& event) const {
        return transport_->queue.Publish(ToProto(session_id_, event));
    }

    EventBroadcaster& queue() const { return transport_->queue; }
    PendingRequests& pending() const { return transport_->pending; }

private:
    std::string session_id_;
    std::shared_ptr<TransportChannels> transport_;
};

}  // namespace minimal::rpc
