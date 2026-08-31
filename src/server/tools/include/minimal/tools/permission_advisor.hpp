#pragma once

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
    AdvisedPermissionChecker(std::shared_ptr<PermissionAdvisor> advisor,
                             std::unique_ptr<PermissionChecker> inner,
                             std::function<bool()> is_cancelled);

    // try advisor; abstain (nullopt) or throw both fall through to the inner
    // checker -- one plugin's failure is not a lost turn. A response returned
    // by the advisor flows through the SAME grant filter at the call site as a
    // human's answer: ToolExecutor re-checks every granted key against the
    // disclosure before remembering it, so an advisor returning an undisclosed
    // key mints nothing.
    PermissionResponse check(const PermissionRequest& request) override;

private:
    std::shared_ptr<PermissionAdvisor> advisor_;
    std::unique_ptr<PermissionChecker> inner_;
    std::function<bool()> is_cancelled_;
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
