#pragma once

// The production agent::SessionTransport: turns each agent::SessionEvent into
// a v1::ServerEvent for every attached subscriber, and owns the pending-request
// rendezvous the wire prompts wait on. Session only ever sees the interface;
// everything wire-shaped happens here, one level removed.
//
// Held by shared_ptr, and holding TransportChannels by shared_ptr: a sink
// captured by a detached watcher thread may outlive both the worker and the
// Session itself, and must then publish into a closed broadcaster harmlessly.

#include <memory>
#include <string>
#include <utility>

#include "minimal/agent/session_event.hpp"
#include "minimal/agent/session_transport.hpp"
#include "minimal/rpc/proto_convert.hpp"
#include "minimal/rpc/transport_channels.hpp"

namespace minimal::rpc {

class EventTranslator : public agent::SessionTransport {
public:
    EventTranslator(std::string session_id, std::shared_ptr<TransportChannels> transport)
        : session_id_(std::move(session_id)), transport_(std::move(transport)) {}

    bool Publish(const agent::SessionEvent& event) override {
        return transport_->queue.Publish(ToProto(session_id_, event));
    }
    void CancelPending() override { transport_->pending.CancelAll(); }
    void ShutdownPending() override { transport_->pending.Shutdown(); }
    void Close() override { transport_->queue.CloseAll(); }

    EventBroadcaster& queue() const { return transport_->queue; }
    PendingRequests& pending() const { return transport_->pending; }

private:
    std::string session_id_;
    std::shared_ptr<TransportChannels> transport_;
};

}  // namespace minimal::rpc
