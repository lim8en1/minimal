#include "minimal/rpc/session_wiring.hpp"

#include <utility>

#include "minimal/rpc/wire_permission_checker.hpp"

namespace minimal::rpc {

void ApplyPermissionAdvisor(agent::SessionParams& params,
                             std::shared_ptr<tools::PermissionAdvisor> advisor,
                             std::chrono::milliseconds advisor_timeout) {
    if (advisor == nullptr) return;
    params.channels = agent::MakeSessionChannels(params.session_id);
    auto cancel_source = std::make_shared<tools::AdvisorCancelSource>();
    params.permission_checker = std::make_unique<tools::AdvisedPermissionChecker>(
        std::move(advisor),
        std::make_unique<WirePermissionChecker>(params.session_id, params.channels->queue,
                                                params.channels->pending),
        cancel_source, advisor_timeout);
    params.advisor_cancel_source = std::move(cancel_source);
}

}  // namespace minimal::rpc
