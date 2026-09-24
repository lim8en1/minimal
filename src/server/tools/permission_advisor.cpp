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

// Ceiling on advisor threads launched and not yet exited (a normal call
// returns to zero; an abandoned one stays counted until its thread finishes).
// Each abandoned thread is self-contained and harmless, but an advisor that
// wedges on EVERY call would otherwise accumulate one thread per Ask,
// unboundedly. Past the ceiling the checker stops spawning: the advisor is
// skipped and the human is prompted directly, which is the same fallback an
// abandoned wait already takes.
constexpr int kMaxAbandonedAdvisors = 8;

// An advisor answers ONE invocation; it never mints session grants. A grant is
// blanket ("this binary, any arguments") and is copied into every subagent,
// so an advisor-minted one let a single confident allow of `git status`
// silence the advisor AND the human for `git push --force` for the rest of
// the session -- including in children that were deliberately left without
// the advisor (advisor_inherit: false). Grants are human-minted only
// (ARCHITECTURE.md "Consent scoping").
PermissionResponse WithoutGrants(PermissionResponse answer) {
    answer.granted_keys.clear();
    return answer;
}

}  // namespace

AdvisedPermissionChecker::AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                                                   std::unique_ptr<PermissionChecker> inner,
                                                   std::function<bool()> is_cancelled,
                                                   std::chrono::milliseconds timeout)
    : advisor_(std::move(advisor)),
      inner_(std::move(inner)),
      cancel_source_(std::make_shared<AdvisorCancelSource>()),
      timeout_(timeout) {
    // A caller-supplied predicate REPLACES the holder's flag-reading default.
    // This shape is for callers that own the cancel semantics themselves
    // (checker-level tests); a Session wired through params shares the holder
    // via the other constructor and fires the flag in Cancel/Terminate.
    if (is_cancelled) cancel_source_->is_cancelled = std::move(is_cancelled);
}

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
            return WithoutGrants(std::move(*answer));
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
    // Already-counted threads (launched, not yet exited) are the ceiling's
    // subject. Past it, spawning yet another thread for an advisor that
    // wedges every call is waste with a side of unbounded accumulation: fall
    // straight through to the wire prompt.
    if (abandoned_count_->load(std::memory_order_relaxed) >= kMaxAbandonedAdvisors) {
        common::DefaultLogger().warn(
            "permission advisor has {} abandoned threads in flight; skipping it for tool '{}'",
            abandoned_count_->load(std::memory_order_relaxed), request.tool_name);
        return inner_->check(request);
    }
    // Advise runs on its own thread so a plugin that ignores the cancellation
    // contract cannot wedge the session worker -- and through it Terminate(),
    // the reaper and process shutdown. The wait polls the cancel source in
    // slices: a cancel abandons the wait immediately, before the budget is
    // spent. The abandoned thread is detached and self-contained -- owned
    // copies of the advisor, cancel source and REQUEST (by value: the
    // caller's reference dies when check returns), never `this` -- so its
    // eventual answer or exception is simply discarded.
    //
    // abandoned_count_ counts each thread from LAUNCH to its eventual exit:
    // incremented right after the thread starts (program order, so the count
    // is exact by the time this function returns on every path), decremented
    // by the task's guard on every exit. Normal completions cancel their own
    // increment (net zero, no drift); only threads abandoned mid-flight stay
    // counted. The earlier shape (increment at abandon, decrement on every
    // exit) drifted the shared counter negative on the normal path, and each
    // drifted decrement then offset a real abandoned increment -- the ceiling
    // leaked.
    auto advisor = advisor_;
    auto cancel_source = cancel_source_;
    auto abandoned_count = abandoned_count_;
    std::packaged_task<std::optional<PermissionResponse>()> task(
        [advisor, cancel_source, abandoned_count, request = request] {
            struct Decrement {
                std::atomic<int>* count;
                ~Decrement() { count->fetch_sub(1, std::memory_order_relaxed); }
            } decrement{abandoned_count.get()};
            return advisor->Advise(request, AdvisorContext{cancel_source->is_cancelled});
        });
    std::future<std::optional<PermissionResponse>> future = task.get_future();
    // Counted BEFORE the thread exists: incrementing after construction let
    // the new thread finish and run its Decrement guard before this line ever
    // ran, transiently driving the shared atomic negative for anything
    // sampling in_flight_advisors() concurrently (nothing was actually broken
    // by it, but it made every such reading a false alarm). Incrementing
    // first closes that window; the catch below still keeps "construction
    // throws -> nothing was counted" true, so a resource-exhaustion failure
    // here still cannot leak the count upward.
    abandoned_count_->fetch_add(1, std::memory_order_relaxed);
    std::thread worker;
    try {
        worker = std::thread(std::move(task));
    } catch (...) {
        abandoned_count_->fetch_sub(1, std::memory_order_relaxed);
        throw;
    }

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
            return WithoutGrants(std::move(*answer));
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
