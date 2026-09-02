#pragma once

#include <string>

// The core-provided signal service plugins reach through ServiceRegistry::
// Find(kSignalServiceName) -- the second CORE-provided service (after
// core.session_support), same shape: header-only pure virtual interface, the
// concrete implementation is SignalRouter (agent/signal_router.hpp), Provided
// in main.cpp before Seal() so a plugin claiming "core.signals" is a
// diagnosed fatal duplicate.
//
// It exists so an INTERNAL producer can be plugin surface: the bundled
// message_session tool is the first consumer, and a third-party producer
// (timers, file watchers, a git-hook bridge) would ship the same way. The
// router's receiver side -- subscriptions, wake, delivery -- stays core; only
// the producing is offered here.
//
// Find returning null means the host does not offer the seam (an embedded or
// test harness): degrade, do not error -- the same contract
// session_support_service.hpp states.

namespace minimal::agent {

inline constexpr const char* kSignalServiceName = "core.signals";

// What happened to one delivered signal. kDelivered requires the addressed
// session to have accepted the message into its injection queue.
enum class SignalDelivery { kDelivered, kNoSubscriber, kInvalid, kDropped };

class SignalService {
public:
    virtual ~SignalService() = default;

    // Deliver one INTERNAL signal from the session named `from_session` to
    // `target_session` -- ADDRESSED, not broadcast: a tool that names its
    // recipient must not reach sessions the recipient never asked about
    // (broadcast-style producers, if they ever arrive, would be their own
    // seam). Requires the target to hold a live "sessions" subscription;
    // the signal is wrapped in the <signal source="sessions" from="...">
    // provenance frame. Outcomes:
    //   - kDelivered: the target accepted the message;
    //   - kNoSubscriber: no such session, it is terminating, or it never
    //     subscribed to "sessions" -- the signal is DROPPED (no queue, no
    //     persistence, no spawn), and the caller says so;
    //   - kInvalid: empty from/target/content, or target == from (self-
    //     address is refused: a signal that arrives without delay is the
    //     wrong tool for a self-reminder -- that is a timer/cron producer's
    //     job).
    virtual SignalDelivery DeliverInternal(const std::string& from_session,
                                           const std::string& target_session,
                                           const std::string& content) = 0;
};

}  // namespace minimal::agent
