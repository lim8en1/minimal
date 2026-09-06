#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "minimal/tools/permission_checker.hpp"

namespace minimal::tools {

// Delivered alongside the request rather than living on PermissionRequest:
// the request is a plain proto-adjacent value (it crosses wire conversion),
// and a std::function member would pollute it. An advisor backed by something
// that can hang (an LLM judge, a remote policy service) must poll this and
// return promptly on cancellation -- the same blocking-safe-but-cancel-aware
// contract every component on the session worker already carries.
struct AdvisorContext {
    std::function<bool()> is_cancelled;
};

// Cancellation reaches an advisor through this holder, because the producer of
// the checker and the owner of the cancel predicate are different objects: the
// params producer composes the advised checker BEFORE the Session exists, but
// only the Session knows when its turn is cancelled or it is terminating. The
// producer creates the source and hands the same shared_ptr to the checker and
// to SessionParams; the Session sets the flag in Cancel()/Terminate().
//
// The flag is also the LIFETIME story. When the budget expires or a cancel
// abandons the wait, the advise thread is DETACHED and keeps running -- it may
// call is_cancelled() long after the Session, the checker and the channels are
// destroyed. The predicate therefore reads this shared atomic and captures
// nothing else: the shared_ptr in the abandoned thread's task lambda keeps the
// flag alive for exactly as long as any poller exists, and "session is gone"
// degrades to whatever the flag last said (Cancel/Terminate set it true; a
// session destroyed without either leaves it false, which is the harmless
// direction -- the poll then costs the advisor nothing but its own contract).
// An earlier draft installed `[this] { return IsCancelled(); }` here: the
// shared_ptr kept the HOLDER alive, not the Session it pointed at, so every
// poll after teardown was a use-after-free.
struct AdvisorCancelSource {
    std::shared_ptr<std::atomic<bool>> cancelled =
        std::make_shared<std::atomic<bool>>(false);
    std::function<bool()> is_cancelled = [flag = cancelled] {
        return flag->load(std::memory_order_acquire);
    };
};

// Answers an Ask automatically, or abstains. nullopt falls through to the
// inner checker -- the wire checker, i.e. the human. An advisor therefore
// narrows what reaches a person; it can never widen what runs, because
// everything below it (mode lattice, precedence chains, ceilings, run modes,
// grant filtering) is enforced by ToolExecutor before the checker is ever
// consulted. See docs/EXTENSIBILITY.md §3.
class PermissionAdvisor {
public:
    virtual ~PermissionAdvisor() = default;
    virtual std::optional<PermissionResponse> Advise(const PermissionRequest& request,
                                                     const AdvisorContext& context) = 0;
};

// Composition, not replacement: ToolExecutor still sees exactly one
// PermissionChecker. Owns the inner checker so the advised decoration carries
// the wire checker's lifetime -- the two are created and destroyed together at
// session setup.
class AdvisedPermissionChecker : public PermissionChecker {
public:
    // Default advisor budget, used when permissions.json does not name one:
    // long enough for a real policy decision, short enough that a wedged
    // external decider converts into a human prompt within a minute.
    static constexpr std::chrono::milliseconds kDefaultTimeout{30000};

    // Owns its cancel source; use when the holder need not be shared (tests).
    // timeout <= 0 disables the budget: Advise runs synchronously, bounded only
    // by the cancellation contract (the pre-timeout behavior, kept for tests).
    AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                             std::unique_ptr<PermissionChecker> inner,
                             std::function<bool()> is_cancelled,
                             std::chrono::milliseconds timeout = kDefaultTimeout);
    // Shares the holder with the Session, whose Cancel/Terminate set the flag.
    AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                             std::unique_ptr<PermissionChecker> inner,
                             std::shared_ptr<AdvisorCancelSource> cancel_source,
                             std::chrono::milliseconds timeout = kDefaultTimeout);

    // try advisor; abstain (nullopt) or throw both fall through to the inner
    // checker -- one plugin's failure is not a lost turn. A response returned
    // by the advisor flows through the SAME grant filter at the call site as a
    // human's answer: ToolExecutor re-checks every granted key against the
    // disclosure before remembering it, so an advisor returning an undisclosed
    // key mints nothing.
    //
    // With a positive timeout, Advise runs on its own thread and the wait
    // polls the cancel source: a wedged advisor (one that ignores the
    // cancellation contract) costs this check its budget -- then the human is
    // prompted -- never the session worker, and through it Terminate, the
    // reaper and process shutdown. An abandoned advisor thread is detached and
    // self-contained (owned copies, no `this`); its discarded answer is
    // harmless. Cancellation abandons the wait immediately, before the budget
    // is spent.
    PermissionResponse check(const PermissionRequest& request) override;

private:
    PermissionResponse CheckDirect(const PermissionRequest& request);
    PermissionResponse CheckWithTimeout(const PermissionRequest& request);

    std::shared_ptr<PermissionAdvisor> advisor_;
    std::unique_ptr<PermissionChecker> inner_;
    std::shared_ptr<AdvisorCancelSource> cancel_source_;
    std::chrono::milliseconds timeout_;
    // Advisor threads abandoned mid-flight (cancelled or timed out, detached,
    // still running). Shared with each task lambda so the thread can decrement
    // it on completion, however long it outlives the checker. See
    // kMaxAbandonedAdvisors in permission_advisor.cpp.
    std::shared_ptr<std::atomic<int>> abandoned_count_ =
        std::make_shared<std::atomic<int>>(0);
};

// Keyed by plugin name; permissions.json's "advisor" field selects one by that
// name. At most one advisor per plugin: a second registration from the same
// plugin is refused with a warning and the first stays in force (the
// AuthProviderRegistry pattern -- a plugin registering twice is a plugin bug,
// not a configuration choice).
//
// Attribution follows the ToolRegistry window pattern: the loader opens a
// scoped window around each plugin's registration callback so registrations
// arriving inside it are stamped with that plugin's name. Deliberately not
// thread-safe during registration, which is single-threaded at startup; Find()
// is read-only afterwards and safe to call from any session-creation path.
class PermissionAdvisorRegistry {
public:
    void SetAttribution(std::string plugin_name) { attribution_ = std::move(plugin_name); }
    void ClearAttribution() { attribution_.clear(); }

    void Register(std::shared_ptr<PermissionAdvisor> advisor);

    // Null when no plugin of that name registered an advisor -- a disabled
    // plugin is a runtime condition, not an error. The startup sanity check
    // turns a configured name that resolves to nothing into a warning.
    std::shared_ptr<PermissionAdvisor> Find(std::string_view plugin_name) const;
    bool Has(std::string_view plugin_name) const { return advisors_.count(std::string(plugin_name)) != 0; }

    struct Attribution {
        std::string plugin_name;
    };
    const std::vector<Attribution>& attributions() const { return attributions_; }

private:
    std::unordered_map<std::string, std::shared_ptr<PermissionAdvisor>> advisors_;
    std::string attribution_;
    std::vector<Attribution> attributions_;
};

}  // namespace minimal::tools
