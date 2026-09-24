#pragma once

// What a Session needs from whatever carries its events to clients, without
// naming the carrier. rpc::EventTranslator is the production implementation
// (gRPC subscribers plus the pending-request rendezvous); a test may supply
// any fake. Held by shared_ptr: background watchers and tool callbacks that
// capture the transport may outlive the Session itself, and must then find
// Publish refusing harmlessly rather than a dangling object.

#include "minimal/agent/session_event.hpp"

namespace minimal::agent {

class SessionTransport {
public:
    virtual ~SessionTransport() = default;

    // Hands one event to every attached client. Never blocks. False when the
    // transport is closed and the event was dropped.
    virtual bool Publish(const SessionEvent& event) = 0;

    // Fails every outstanding client prompt (permission, confirmation, ask)
    // as "no answer" -- Cancel(): the turn is being abandoned.
    virtual void CancelPending() = 0;
    // As CancelPending, and refuses every future prompt -- Terminate().
    virtual void ShutdownPending() = 0;
    // Closes every client stream after the terminal event was published.
    virtual void Close() = 0;
};

}  // namespace minimal::agent
