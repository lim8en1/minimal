#include "minimal/rpc/wire_permission_checker.hpp"

#include <utility>

#include "minimal/rpc/proto_convert.hpp"

namespace minimal::rpc {

WirePermissionChecker::WirePermissionChecker(std::string session_id,
                                             EventQueue<v1::ServerEvent>& queue,
                                             PendingRequests& pending)
    : session_id_(std::move(session_id)), queue_(queue), pending_(pending) {}

std::string WirePermissionChecker::NextRequestId() {
    // Unique within the session, which is all correlation needs: request_id is
    // only ever matched against this session's own PendingRequests, and
    // ServerEvent already carries session_id alongside it.
    return "perm-" + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
}

tools::PermissionResponse WirePermissionChecker::check(const tools::PermissionRequest& request) {
    const std::string request_id = NextRequestId();

    // REGISTER BEFORE PUBLISHING. The client can answer the instant the event is
    // written, so the slot has to exist first -- otherwise a fast answer arrives
    // with nowhere to go and this call blocks forever on a question that was
    // already answered.
    auto registration = pending_.Register(request_id);

    v1::ServerEvent event;
    event.set_session_id(session_id_);
    auto* permission = event.mutable_permission_request();
    *permission = ToProto(request);
    permission->set_request_id(request_id);
    queue_.Push(std::move(event));

    const std::optional<std::string> answer = registration.AwaitResponse();
    if (!answer) {
        // Cancelled: the session is terminating. Deny is the correct outcome --
        // the work is being torn down, so the tool must not run -- but it is a
        // decision made HERE, not inside the synchronisation primitive, which
        // deliberately reports only "no answer".
        return tools::PermissionResponse{tools::PermissionDecision::Deny, {}};
    }

    // The reader hands the serialized PermissionResponse across as the resolved
    // value. Parsing it here rather than at the dispatch site keeps the reader
    // thread free of anything that could fail or allocate meaningfully.
    v1::PermissionResponse decoded;
    if (!decoded.ParseFromString(*answer)) {
        return tools::PermissionResponse{tools::PermissionDecision::Deny, {}};
    }
    return FromProto(decoded, request.disclosure);
}

}  // namespace minimal::rpc
