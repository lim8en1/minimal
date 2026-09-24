#pragma once

// tools::AskBroker over the bidi stream.
//
// The ask-side twin of WireConfirmationBroker; WirePrompt does the
// register/publish/await round trip. Testable in-process with no channel.

#include <chrono>
#include <string>

#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/rpc/wire_prompt.hpp"
#include "minimal/tools/ask.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WireAskBroker : public tools::AskBroker {
public:
    // unattended_timeout: sessions.unattended_prompt_timeout_seconds, see
    // WirePermissionChecker. Zero (the default) waits forever.
    WireAskBroker(std::string session_id, EventBroadcaster& queue, PendingRequests& pending,
                  std::chrono::milliseconds unattended_timeout = {});

    // Blocks the worker until the client answers, the turn is cancelled, the
    // session terminates, or (if configured) the unattended timeout elapses.
    // Every one of those -- and every shape-invalid answer -- is
    // `answered == false`: this broker never invents a selection, the same
    // fail-closed rule RequestConfirmation applies to unoffered options.
    tools::AskAnswer Ask(const tools::AskRequest& request) override;

private:
    WirePrompt prompt_;
};

}  // namespace minimal::rpc
