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

commands::ConfirmationResponse WireConfirmationBroker::RequestConfirmation(
    const commands::ConfirmationRequest& request) {
    const std::string request_id = request_ids_.Next();

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

    // Cancelled, terminating, timed out, or a PermissionAnswer delivered
    // against this id -- a client answering the wrong kind of request, which
    // AwaitTypedAnswer's variant narrowing makes structurally unreadable
    // rather than something this code has to detect by hand. Every case is
    // "no answer, not a choice", the same fail-closed outcome an unoffered
    // option gets below.
    const std::optional<ConfirmationAnswer> confirmation =
        AwaitTypedAnswer<ConfirmationAnswer>(registration, unattended_timeout_);
    if (!confirmation) return {};

    // Only an option that was OFFERED counts. A client cannot invent one, for
    // the same reason granted_keys is filtered against what the permission
    // prompt offered: the server decides what the choices were.
    for (const auto& option : request.options) {
        if (confirmation->chosen_option == option) return commands::ConfirmationResponse{option};
    }
    return {};
}

}  // namespace minimal::rpc
