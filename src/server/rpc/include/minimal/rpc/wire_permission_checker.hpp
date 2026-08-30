#pragma once

// The first real PermissionChecker: it asks a human, over the wire.
//
// Until now only test fakes implemented this interface, which is why the
// blocking contract in permission_checker.hpp had never actually been exercised.
// This is where it becomes load-bearing -- check() runs on the session worker
// and blocks there, which is safe ONLY because the gRPC reader thread keeps
// draining and can deliver the answer that unblocks it.
//
// Deliberately free of gRPC. It publishes onto the session's EventQueue and
// waits on PendingRequests; who drains that queue is the transport's problem.
// That keeps the permission round-trip testable in-process, against the real
// implementation rather than a stand-in for it.

#include <atomic>
#include <cstdint>
#include <string>

#include "minimal/rpc/event_queue.hpp"
#include "minimal/rpc/pending_requests.hpp"
#include "minimal/tools/permission_checker.hpp"
#include "minimal/v1/session_service.pb.h"

namespace minimal::rpc {

class WirePermissionChecker : public tools::PermissionChecker {
public:
    WirePermissionChecker(std::string session_id, EventQueue<v1::ServerEvent>& queue,
                          PendingRequests& pending);

    // Blocks the calling (session worker) thread until answered or cancelled.
    tools::PermissionResponse check(const tools::PermissionRequest& request) override;

private:
    std::string NextRequestId();

    std::string session_id_;
    EventQueue<v1::ServerEvent>& queue_;
    PendingRequests& pending_;
    std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace minimal::rpc
