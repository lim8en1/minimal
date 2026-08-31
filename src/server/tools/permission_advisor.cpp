#include "minimal/tools/permission_advisor.hpp"

#include <chrono>
#include <future>
#include <thread>
#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::tools {
namespace {

// The poll slice of the bounded wait: small enough that a cancel or terminate
// unblocks the session worker promptly, coarse enough not to matter at human
// decision scale.
constexpr std::chrono::milliseconds kWaitSlice{25};

}  // namespace

AdvisedPermissionChecker::AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                                                   std::unique_ptr<PermissionChecker> inner,
                                                   std::function<bool()> is_cancelled,
                                                   std::chrono::milliseconds timeout)
    : advisor_(std::move(advisor)),
      inner_(std::move(inner)),
      cancel_source_(std::make_shared<AdvisorCancelSource>(
          AdvisorCancelSource{std::move(is_cancelled)})),
      timeout_(timeout) {}

AdvisedPermissionChecker::AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                                                   std::unique_ptr<PermissionChecker> inner,
                                                   std::shared_ptr<AdvisorCancelSource> cancel_source,
                                                   std::chrono::milliseconds timeout)
    : advisor_(std::move(advisor)),
      inner_(std::move(inner)),
      cancel_source_(std::move(cancel_source)),
      timeout_(timeout) {
    if (!cancel_source_) {
        cancel_source_ = std::make_shared<AdvisorCancelSource>();
    }
}

PermissionResponse AdvisedPermissionChecker::check(const PermissionRequest& request) {
    if (timeout_ > std::chrono::milliseconds::zero()) return CheckWithTimeout(request);
    return CheckDirect(request);
}

PermissionResponse AdvisedPermissionChecker::CheckDirect(const PermissionRequest& request) {
    AdvisorContext context{cancel_source_->is_cancelled};
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

PermissionResponse AdvisedPermissionChecker::CheckWithTimeout(const PermissionRequest& request) {
    // Advise runs on its own thread so a plugin that ignores the cancellation
    // contract cannot wedge the session worker -- and through it Terminate(),
    // the reaper and process shutdown. The wait polls the cancel source in
    // slices: a cancel abandons the wait immediately, before the budget is
    // spent. The abandoned thread is detached and self-contained -- owned
    // copies of the advisor, cancel source and REQUEST (by value: the
    // caller's reference dies when check returns), never `this` -- so its
    // eventual answer or exception is simply discarded.
    auto advisor = advisor_;
    auto cancel_source = cancel_source_;
    std::packaged_task<std::optional<PermissionResponse>()> task(
        [advisor, cancel_source, request = request] {
            return advisor->Advise(request, AdvisorContext{cancel_source->is_cancelled});
        });
    std::future<std::optional<PermissionResponse>> future = task.get_future();
    std::thread worker(std::move(task));

    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    for (;;) {
        if (future.wait_for(kWaitSlice) == std::future_status::ready) break;
        if (cancel_source->is_cancelled()) {
            worker.detach();
            common::DefaultLogger().warn(
                "permission advisor still running at cancellation for tool '{}'; abandoning it "
                "and falling through to the wire prompt",
                request.tool_name);
            return inner_->check(request);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            worker.detach();
            common::DefaultLogger().warn(
                "permission advisor timed out after {}ms for tool '{}'; falling through to the "
                "wire prompt",
                std::chrono::duration_cast<std::chrono::milliseconds>(timeout_).count(),
                request.tool_name);
            return inner_->check(request);
        }
    }
    worker.join();

    try {
        if (auto answer = future.get()) {
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
