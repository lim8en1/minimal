#pragma once

// The first real PermissionChecker: it asks a human, over the wire.
//
// Until now only test fakes implemented this interface, which is why the
// blocking contract in permission_checker.hpp had never actually been exercised.
// This is where it becomes load-bearing -- check() runs on the session worker
// and blocks there, which is safe ONLY because the gRPC reader thread keeps
// draining and can deliver the answer that unblocks it.
//
// Deliberately free of gRPC. It publishes onto the session's event broadcaster
// and waits on PendingRequests; who drains that queue is the transport's problem.
// That keeps the permission round-trip testable in-process, against the real
// implementation rather than a stand-in for it.

#include <chrono>
#include <optional>
#include <string>

#include "minimal/rpc/event_broadcaster.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/rpc/wire_prompt.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WirePermissionChecker : public tools::PermissionChecker {
public:
    // unattended_timeout: sessions.unattended_prompt_timeout_seconds. Zero
    // (the default) waits forever, unchanged from before S5 -- a detached
    // session opts into a bound explicitly.
    WirePermissionChecker(std::string session_id, EventBroadcaster& queue,
                          PendingRequests& pending,
                          std::chrono::milliseconds unattended_timeout = {});

    // Blocks the calling (session worker) thread until answered, cancelled,
    // or (if configured) the unattended timeout elapses.
    tools::PermissionResponse check(const tools::PermissionRequest& request) override;

private:
    WirePrompt prompt_;
};

}  // namespace minimal::rpc
