#pragma once

// tools::PromptBroker over the bidi stream.
//
// The plugin-prompt twin of WireConfirmationBroker; WirePrompt does the
// register/publish/await round trip. Testable in-process with no channel.

#include <chrono>
#include <string>

#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/rpc/wire_prompt.hpp"
#include "minimal/tools/prompt.hpp"
#include "minimal/v1/session_service.pb.h"
#include "minimal/common/units.hpp"
#include "minimal/rpc/policy.hpp"

namespace minimal::rpc {

class WirePluginPromptBroker : public tools::PromptBroker {
public:
    // unattended_timeout: sessions.unattended_prompt_timeout_seconds, see
    // WirePermissionChecker. Zero (the default) waits forever.
    WirePluginPromptBroker(std::string session_id, EventBroadcaster& queue,
                           PendingRequests& pending,
                           std::chrono::milliseconds unattended_timeout = {});

    // Blocks the worker until the client answers, the turn is cancelled, the
    // session terminates, or (if configured) the unattended timeout elapses.
    // The payload is serialized as-is and the answer parsed as-is: this
    // broker knows no prompt shape. An ANSWERED response whose answer_json
    // does not parse, or is over kMaxPluginPromptAnswerBytes, reads as Dismissed -- never
    // as an answer the tool would have to second-guess.
    tools::PromptResult Prompt(const tools::PromptRequest& request) override;

private:
    WirePrompt prompt_;
};

}  // namespace minimal::rpc
