#include "minimal/rpc/wire_permission_checker.hpp"

#include <algorithm>
#include <utility>

#include "minimal/common/logger.hpp"
#include "minimal/rpc/proto_convert.hpp"

namespace minimal::rpc {

WirePermissionChecker::WirePermissionChecker(std::string session_id,
                                             EventBroadcaster& queue,
                                             PendingRequests& pending,
                                             std::chrono::milliseconds unattended_timeout)
    : prompt_(std::move(session_id), queue, pending, unattended_timeout, "perm-") {}

tools::PermissionResponse WirePermissionChecker::check(const tools::PermissionRequest& request) {
    // No usable answer (cancelled, terminating, timed out, or the wrong kind
    // of answer) is a deny -- decided here, not in WirePrompt, which only
    // reports that nothing usable arrived.
    const std::optional<PermissionAnswer> answer = prompt_.Ask<PermissionAnswer>(
        [&request](v1::ServerEvent& event, const std::string& request_id) {
            auto* permission = event.mutable_permission_request();
            *permission = ToProto(request);
            permission->set_request_id(request_id);
        });
    if (!answer) {
        return tools::PermissionResponse{tools::PermissionDecision::Deny, {}};
    }
    // A grant key is only meaningful as the answer to something THIS request
    // offered -- filtered here, not at the reader, because only the request
    // that built the disclosure knows what it offered. Taking arbitrary keys
    // off the wire would let a client mint permissions for binaries no prompt
    // ever showed a human.
    tools::PermissionResponse out;
    out.decision = answer->decision;
    out.deny_message = answer->deny_message;
    out.stop_turn = answer->stop_turn;
    for (const auto& key : answer->granted_keys) {
        const bool was_offered =
            std::any_of(request.disclosure.grants.begin(), request.disclosure.grants.end(),
                        [&key](const tools::GrantOffer& offer) { return offer.key == key; });
        if (was_offered) {
            out.granted_keys.push_back(key);
        } else {
            common::DefaultLogger().warn("wire_permission_checker: ignoring ungranted key '{}'", key);
        }
    }
    return out;
}

}  // namespace minimal::rpc
