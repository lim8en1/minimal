#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

// The core-provided signal service plugins reach through ServiceRegistry::
// Find(kSignalServiceName) -- the second CORE-provided service (after
// core.session_support), same shape: header-only pure virtual interface, the
// concrete implementation is SignalRouter (agent/signal_router.hpp), Provided
// in main.cpp before Seal() so a plugin claiming "core.signals" is a
// diagnosed fatal duplicate.
//
// Two producer families reach the router through it, and both are plugin
// surface:
//
//   - INTERNAL: cross-session messages on the builtin "sessions" source
//     (DeliverInternal). The bundled message_session tool is the first
//     consumer; a third-party producer (timers, file watchers, a git-hook
//     bridge) would ship the same way.
//   - EXTERNAL: named channels an operator declared, whose events arrive
//     from outside the process (RegisterExternalChannel + DeliverExternal).
//     The MCP signal channels (channels.json) are the first such producer:
//     the transport and its wire protocol are the producer's business, the
//     router sees only (channel, signal, content, meta).
//
// The receiver side -- subscriptions, the restricted/extended allowlist,
// provenance framing, wake, delivery -- stays core: a producer can PRODUCE
// signals, never change who RECEIVES them.
//
// Find returning null means the host does not offer the seam (an embedded or
// test harness): degrade, do not error -- the same contract
// session_support_service.hpp states.

namespace minimal::agent {

inline constexpr const char* kSignalServiceName = "core.signals";

// The builtin INTERNAL source (cross-session messages). Never an external
// channel: RegisterExternalChannel refuses the name.
inline constexpr const char* kInternalSignalSource = "sessions";

// The one name rule for channel names, signal names and meta keys -- both
// halves of a "channel/signal" selector: lowercase alphanumerics plus '.',
// '_' and '-'. One charset everywhere means a selector is unambiguously split
// on the FIRST '/', and no case-confusion can smuggle a near-miss name past
// a restricted allowlist. Producers validate their own config against it.
inline bool IsValidSignalName(std::string_view name) {
    if (name.empty()) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// One external channel as the router needs it: its name and its allowlist.
// Everything about HOW its events arrive (a process to spawn, a wire
// protocol) stays with the producer that registers it.
struct ExternalChannel {
    std::string name;
    // restricted (false): only `signals` may be delivered, and sessions
    // subscribe per signal ("ci/build.completed"). extended (true): any
    // signal name passes, and sessions subscribe at channel level.
    bool extended = false;
    std::vector<std::string> signals;
    // The registering plugin. Its permission model gates each signal per
    // receiving session (PermissionModel::ResolveSubject with kind "signal",
    // key "<channel>/<signal>"), checked when the session's worker takes the
    // message; no rule = delivered (the subscription is the consent). Empty:
    // ungated.
    std::string owner;
};

// Implemented by the producer of external channels. The router calls it when
// a session subscribes, so a subscription to a channel that cannot come up is
// refused at session start instead of looking like "no signal ever arrives".
// Called on the session-launch path; may block for the producer's own connect
// budget.
class ExternalChannelSource {
public:
    virtual ~ExternalChannelSource() = default;
    // True when `channel`'s producer is up (starting it if needed); false
    // with `error` set when it cannot be.
    virtual bool EnsureAlive(const std::string& channel, std::string& error) = 0;
};

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

    // --- External ingress ---------------------------------------------------

    // Declares an external channel sessions may subscribe to. At startup,
    // before serving. `source` (borrowed; may be null for a structural-only
    // host) must outlive serving. Refused, with `error` set, for an invalid
    // or reserved name ("sessions"), a restricted channel without signals,
    // an extended channel WITH a list, or a name already registered.
    virtual bool RegisterExternalChannel(const ExternalChannel& channel,
                                         ExternalChannelSource* source, std::string& error) = 0;

    // The channel's own description of what its signals mean (the MCP
    // initialize instructions), rendered into subscribing sessions' context.
    // Unregistered channels store nothing.
    virtual void SetChannelInstructions(const std::string& channel,
                                        const std::string& instructions) = 0;

    // Delivers one signal from a registered external channel to every
    // session subscribed to it. Non-blocking (safe on a producer's read
    // thread) and never throws. The router validates the signal name,
    // enforces a restricted channel's allowlist, sanitizes `meta` into
    // attributes, and wraps the untrusted `content` in its provenance frame.
    // kInvalid: unregistered channel, bad signal name, or a restricted
    // channel's undeclared signal -- each logged, like every drop.
    virtual SignalDelivery DeliverExternal(const std::string& channel, const std::string& signal,
                                           const std::string& content,
                                           const std::map<std::string, std::string>& meta) = 0;
};

}  // namespace minimal::agent
