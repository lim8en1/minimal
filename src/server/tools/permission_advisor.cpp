#include "minimal/tools/permission_advisor.hpp"

#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::tools {

AdvisedPermissionChecker::AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                                                   std::unique_ptr<PermissionChecker> inner,
                                                   std::function<bool()> is_cancelled)
    : advisor_(std::move(advisor)),
      inner_(std::move(inner)),
      is_cancelled_(std::move(is_cancelled)) {}

PermissionResponse AdvisedPermissionChecker::check(const PermissionRequest& request) {
    AdvisorContext context{is_cancelled_};
    try {
        if (auto answer = advisor_->Advise(request, context)) {
            common::DefaultLogger().info(
                "permission advisor answered {} for tool '{}'",
                answer->decision == PermissionDecision::Allow ? "allow" : "deny",
                request.tool_name);
            return *answer;
        }
    } catch (const std::exception& e) {
        common::DefaultLogger().warn(
            "permission advisor threw for tool '{}'; falling through to the wire prompt ({})",
            request.tool_name, e.what());
    } catch (...) {
        common::DefaultLogger().warn(
            "permission advisor threw an unknown exception for tool '{}'; falling through to the "
            "wire prompt",
            request.tool_name);
    }
    return inner_->check(request);
}

void PermissionAdvisorRegistry::Register(std::shared_ptr<PermissionAdvisor> advisor) {
    if (attribution_.empty()) {
        // Core itself never registers an advisor; reaching here means a plugin
        // registered outside a loader attribution window, which would leave the
        // advisor unnameable from permissions.json. Refuse rather than mis-attribute.
        common::DefaultLogger().warn(
            "permission advisor registered outside an attribution window; refused");
        return;
    }
    const std::string name = attribution_;
    if (advisors_.count(name) != 0) {
        common::DefaultLogger().warn(
            "plugin '{}' registered a second permission advisor; the first stays in force", name);
        return;
    }
    advisors_.emplace(name, std::move(advisor));
    attributions_.push_back(Attribution{name});
}

std::shared_ptr<PermissionAdvisor> PermissionAdvisorRegistry::Find(
    std::string_view plugin_name) const {
    auto it = advisors_.find(std::string(plugin_name));
    return it == advisors_.end() ? nullptr : it->second;
}

}  // namespace minimal::tools
