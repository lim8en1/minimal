#include "minimal/rpc/wire_permission_checker.hpp"

#include <utility>

#include "minimal/rpc/proto_convert.hpp"

namespace minimal::rpc {

WirePermissionChecker::WirePermissionChecker(std::string session_id,
                                             EventBroadcaster& queue,
                                             PendingRequests& pending,
                                             std::chrono::milliseconds unattended_timeout)
    : session_id_(std::move(session_id)),
      queue_(queue),
      pending_(pending),
      unattended_timeout_(unattended_timeout) {}

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
    // Publish never blocks (event_broadcaster.hpp): a full subscriber drops
    // its own oldest, so this call cannot be wedged by a stalled client any
    // more -- the old two-file dependency on Terminate's Close ordering is
    // gone. Every attached subscriber gets the prompt; first answer wins.
    queue_.Publish(std::move(event));

    const std::optional<PendingAnswer> registration_answer = registration.AwaitResponse(
        unattended_timeout_ > std::chrono::milliseconds::zero()
            ? std::optional(unattended_timeout_)
            : std::nullopt);
    if (!registration_answer) {
        // Cancelled, terminating, or (if sessions.unattended_prompt_timeout_
        // seconds is set) timed out with nobody attached to answer. Deny is
        // the correct outcome in every one of those cases -- the work is
        // being torn down or nobody is there to grant it -- but it is a
        // decision made HERE, not inside the synchronisation primitive, which
        // deliberately reports only "no answer" for all three.
        return tools::PermissionResponse{tools::PermissionDecision::Deny, {}};
    }

    // The reader hands the serialized PermissionResponse across as the typed
    // variant. A ConfirmationAnswer landing here -- a client answering the
    // wrong kind of request -- is structurally unreadable (get_if returns
    // null), which is the same fail-closed outcome a malformed payload had.
    // Parsing it HERE rather than at the dispatch site keeps the reader thread
    // free of anything that could fail or allocate meaningfully.
    const auto* answer = std::get_if<PermissionAnswer>(&*registration_answer);
    if (answer == nullptr) {
        return tools::PermissionResponse{tools::PermissionDecision::Deny, {}};
    }
    v1::PermissionResponse decoded;
    if (!decoded.ParseFromString(answer->serialized_response)) {
        return tools::PermissionResponse{tools::PermissionDecision::Deny, {}};
    }
    return FromProto(decoded, request.disclosure);
}

}  // namespace minimal::rpc
