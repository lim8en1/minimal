#include "minimal/rpc/wire_confirmation_broker.hpp"

#include <utility>

namespace minimal::rpc {

WireConfirmationBroker::WireConfirmationBroker(std::string session_id,
                                               EventBroadcaster& queue,
                                               PendingRequests& pending,
                                               std::chrono::milliseconds unattended_timeout)
    : session_id_(std::move(session_id)),
      queue_(queue),
      pending_(pending),
      unattended_timeout_(unattended_timeout) {}

std::string WireConfirmationBroker::NextRequestId() {
    return "confirm-" + std::to_string(next_id_.fetch_add(1, std::memory_order_relaxed));
}

commands::ConfirmationResponse WireConfirmationBroker::RequestConfirmation(
    const commands::ConfirmationRequest& request) {
    const std::string request_id = NextRequestId();

    // Before the push, always -- see the header.
    auto registration = pending_.Register(request_id);

    v1::ServerEvent event;
    event.set_session_id(session_id_);
    auto* wire = event.mutable_confirmation_request();
    wire->set_request_id(request_id);
    wire->set_kind(request.kind);
    wire->set_title(request.title);
    wire->set_detail(request.detail);
    for (const auto& option : request.options) wire->add_options(option);
    queue_.Publish(std::move(event));

    const auto answer = registration.AwaitResponse(
        unattended_timeout_ > std::chrono::milliseconds::zero()
            ? std::optional(unattended_timeout_)
            : std::nullopt);
    if (!answer) return {};  // cancelled, terminating, or timed out: no answer, not a choice

    // Typed variant: a PermissionAnswer delivered against this id -- a client
    // answering the wrong kind of request -- is structurally unreadable here,
    // the same fail-closed outcome an unoffered option gets below.
    const auto* confirmation = std::get_if<ConfirmationAnswer>(&*answer);
    if (confirmation == nullptr) return {};

    // Only an option that was OFFERED counts. A client cannot invent one, for
    // the same reason granted_keys is filtered against what the permission
    // prompt offered: the server decides what the choices were.
    for (const auto& option : request.options) {
        if (confirmation->chosen_option == option) return commands::ConfirmationResponse{option};
    }
    return {};
}

}  // namespace minimal::rpc
