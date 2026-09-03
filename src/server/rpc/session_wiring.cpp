#include "minimal/rpc/session_wiring.hpp"

#include <utility>

#include "minimal/rpc/event_translator.hpp"
#include "minimal/rpc/transport_channels.hpp"
#include "minimal/rpc/wire_confirmation_broker.hpp"
#include "minimal/rpc/wire_permission_checker.hpp"

namespace minimal::rpc {
namespace {

// Builds a fresh TransportChannels + EventTranslator and wires every
// transport-shaped SessionParams field from it. Shared by
// ApplyPermissionAdvisor (early) and FinishSessionWiring (late, only when
// ApplyPermissionAdvisor did not already run this for the session).
std::shared_ptr<EventTranslator> BuildTransport(agent::SessionParams& params) {
    auto transport = MakeTransportChannels(params.session_id);
    auto translator = std::make_shared<EventTranslator>(params.session_id, transport);
    params.transport_handle = translator;
    params.event_sink = *translator;
    // Captured by value: the callback must work after the producer that
    // built it (and Session itself) may be gone, same lifetime rule as
    // EventTranslator's own copies.
    params.cancel_pending_requests = [transport] { transport->pending.CancelAll(); };
    params.shutdown_pending_requests = [transport] { transport->pending.Shutdown(); };
    params.close_transport = [transport] { transport->queue.CloseAll(); };
    return translator;
}

}  // namespace

void ApplyPermissionAdvisor(agent::SessionParams& params,
                             std::shared_ptr<tools::PermissionAdvisor> advisor,
                             std::chrono::milliseconds advisor_timeout) {
    if (advisor == nullptr) return;
    auto translator = BuildTransport(params);
    auto cancel_source = std::make_shared<tools::AdvisorCancelSource>();
    params.permission_checker = std::make_unique<tools::AdvisedPermissionChecker>(
        std::move(advisor),
        std::make_unique<WirePermissionChecker>(params.session_id, translator->queue(),
                                                translator->pending()),
        cancel_source, advisor_timeout);
    params.advisor_cancel_source = std::move(cancel_source);
}

void FinishSessionWiring(agent::SessionParams& params) {
    std::shared_ptr<EventTranslator> translator =
        params.transport_handle ? std::static_pointer_cast<EventTranslator>(params.transport_handle)
                                : BuildTransport(params);

    if (!params.permission_checker) {
        params.permission_checker = std::make_unique<WirePermissionChecker>(
            params.session_id, translator->queue(), translator->pending(),
            params.unattended_prompt_timeout);
    }
    params.confirmation_broker = std::make_unique<WireConfirmationBroker>(
        params.session_id, translator->queue(), translator->pending(),
        params.unattended_prompt_timeout);
}

}  // namespace minimal::rpc
