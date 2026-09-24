#include "minimal/rpc/wire_confirmation_broker.hpp"

#include <utility>

namespace minimal::rpc {

WireConfirmationBroker::WireConfirmationBroker(std::string session_id,
                                               EventBroadcaster& queue,
                                               PendingRequests& pending,
                                               std::chrono::milliseconds unattended_timeout)
    : prompt_(std::move(session_id), queue, pending, unattended_timeout, "confirm-") {}

commands::ConfirmationResponse WireConfirmationBroker::RequestConfirmation(
    const commands::ConfirmationRequest& request) {
    // No usable answer is "no answer, not a choice" -- the same fail-closed
    // outcome an unoffered option gets below.
    const std::optional<ConfirmationAnswer> confirmation = prompt_.Ask<ConfirmationAnswer>(
        [&request](v1::ServerEvent& event, const std::string& request_id) {
            auto* wire = event.mutable_confirmation_request();
            wire->set_request_id(request_id);
            wire->set_kind(request.kind);
            wire->set_title(request.title);
            wire->set_detail(request.detail);
            for (const auto& option : request.options) wire->add_options(option);
        });
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
