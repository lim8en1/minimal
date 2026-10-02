#include "minimal/rpc/session_wiring.hpp"

#include <stdexcept>
#include <utility>

#include "minimal/rpc/event_translator.hpp"
#include "minimal/rpc/transport_channels.hpp"
#include "minimal/rpc/wire_confirmation_broker.hpp"
#include "minimal/rpc/wire_plugin_prompt_broker.hpp"
#include "minimal/rpc/wire_permission_checker.hpp"

namespace minimal::rpc {
namespace {

// A fresh TransportChannels + EventTranslator, unless the producer already
// supplied the transport (a test injecting its own).
std::shared_ptr<EventTranslator> BuildTransport(agent::SessionParams& params) {
    auto translator = std::make_shared<EventTranslator>(params.session_id,
                                                        MakeTransportChannels(params.session_id));
    params.transport = translator;
    return translator;
}

// The advisor-composition recipe: wraps a WirePermissionChecker (the human
// fallback, bounded by the same unattended timeout as every other prompt) in an
// AdvisedPermissionChecker. File-local now that FinishSessionWiring is its
// only caller.
void ApplyPermissionAdvisor(agent::SessionParams& params,
                             std::shared_ptr<tools::PermissionAdvisor> advisor,
                             std::chrono::milliseconds advisor_timeout,
                             std::chrono::milliseconds unattended_timeout,
                             const std::shared_ptr<EventTranslator>& translator) {
    auto cancel_source = std::make_shared<tools::AdvisorCancelSource>();
    params.permission_checker = std::make_unique<tools::AdvisedPermissionChecker>(
        std::move(advisor),
        std::make_unique<WirePermissionChecker>(params.session_id, translator->queue(),
                                                translator->pending(), unattended_timeout),
        cancel_source, advisor_timeout);
    params.advisor_cancel_source = std::move(cancel_source);
}

}  // namespace

void FinishSessionWiring(agent::SessionParams& params) {
    std::shared_ptr<EventTranslator> translator =
        params.transport ? std::dynamic_pointer_cast<EventTranslator>(params.transport)
                         : BuildTransport(params);
    if (translator == nullptr) {
        throw std::invalid_argument("FinishSessionWiring needs an rpc::EventTranslator transport");
    }

    if (params.selected_advisor) {
        ApplyPermissionAdvisor(params, std::move(params.selected_advisor), params.advisor_timeout,
                               params.unattended_prompt_timeout, translator);
    } else if (!params.permission_checker) {
        params.permission_checker = std::make_unique<WirePermissionChecker>(
            params.session_id, translator->queue(), translator->pending(),
            params.unattended_prompt_timeout);
    }
    params.confirmation_broker = std::make_unique<WireConfirmationBroker>(
        params.session_id, translator->queue(), translator->pending(),
        params.unattended_prompt_timeout);
    params.prompt_broker = std::make_unique<WirePluginPromptBroker>(
        params.session_id, translator->queue(), translator->pending(),
        params.unattended_prompt_timeout);
}

}  // namespace minimal::rpc
