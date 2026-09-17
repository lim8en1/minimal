#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "minimal/agent/signal_service.hpp"
#include "minimal/config/channels_config.hpp"

namespace minimal::agent {

class Session;

// Routes signals to subscribed sessions -- the ONE receiver path for every
// producer. A session receives ONLY the signals it subscribed to at start;
// a signal with no matching subscription is dropped with a log line, never
// queued for later and never allowed to start work on its own.
//
// Two producer families share this path:
//
//   - EXTERNAL: an MCP channel server (channels.json, stdio) pushes
//     notifications/minimal/signal. OnMcpNotification() is invoked with the
//     raw JSON-RPC line -- on the channel's read thread, so it must stay
//     non-blocking (parse, map lookup, InjectionQueue::Push: all bounded).
//
//   - INTERNAL: same-process sessions messaging each other (the bundled
//     message_session tool today; future producers reach this through the
//     core.signals service). No MCP anywhere: DeliverInternal() goes straight
//     to the builtin "sessions" source. The trust story differs from external
//     channels -- same local user's agents, the sender gated by the tool
//     permission model, the receiver by its own subscription -- which is why
//     internal signals have no mode and no channels.json presence.
//
// The router holds sessions by weak_ptr: a terminated or destroyed session
// simply stops matching (and QueueInjectedMessage refuses on terminated_
// besides), so teardown needs no unregistration handshake to stay correct.
//
// Core, not a plugin, for the same reason SubagentManager is: delivery
// crosses session boundaries and needs registry lifetime, and the
// subscription surface is SessionParams/proto. Producers, by contrast, are
// plugin surface -- the router IS the core.signals service (SignalService),
// so a plugin tool can Deliver without privileged linkage.
class SignalRouter : public SignalService {
public:
    // What happened to one delivered signal. Delivered requires at least one
    // live subscriber to have accepted the message into its injection queue.
    using Delivery = SignalDelivery;
    // The channel registry, from channels.json, loaded once at startup.
    // Subscription validation is purely structural against it -- a selector
    // never needs the channel to be connected, only declared. Calling twice
    // for one name replaces the entry (idempotent startup).
    void RegisterChannel(const config::ChannelEntry& entry);

    // A channel server's own description of what its signals mean, captured
    // from the initialize result when the manager connects it. Consumed by
    // SignalContextFor(): an empty string contributes nothing. Unknown
    // channels (never registered) store nothing.
    void SetChannelInstructions(const std::string& channel, const std::string& instructions);

    // EXTERNAL producer. `raw` is one complete JSON-RPC notification line as
    // the channel server wrote it. Only kSignalNotificationMethod frames are
    // signal candidates; anything else (tools/list_changed, ...) is ignored
    // silently -- an MCP server may send other notifications for its own
    // reasons. Never throws.
    void OnMcpNotification(const std::string& channel, const std::string& raw);

    // INTERNAL producer (the SignalService override): addressed delivery to
    // `target_session`'s "sessions" subscription. The content is wrapped with
    // a from= attribute so the receiving model can tell agent output from
    // human input. Self-address is refused (kInvalid): a signal that arrives
    // without delay is the wrong tool for a self-reminder -- that is a
    // timer/cron producer's job.
    SignalDelivery DeliverInternal(const std::string& from_session,
                                   const std::string& target_session,
                                   const std::string& content) override;

    // Subscription. `selector` is "channel" or "channel/signal":
    //   - restricted channel: per-signal selectors only, and the signal must
    //     be in the declared list (a session that starts against a stale or
    //     renamed signal gets INVALID_ARGUMENT at start, not silence);
    //   - extended channel: channel-level selectors only -- extended means
    //     every signal passes, so a per-signal filter there would be theater;
    //   - the builtin "sessions" source: channel-level ("sessions").
    // Returns false with `error` set on any malformed or undeclared selector.
    bool Subscribe(std::shared_ptr<Session> session, const std::string& selector,
                   std::string& error);

    // The validation half of Subscribe, usable BEFORE a session exists: the
    // session service checks every requested selector (and warms the
    // channels' connections) at session start, then subscribes the created
    // session. Same rules, same errors. `channel` (when non-null) receives
    // the selector's channel part, so the caller can EnsureAlive exactly the
    // channels it needs.
    bool CheckSelector(const std::string& selector, std::string& error,
                       std::string* channel = nullptr) const;

    // Explicit drop of one session's subscriptions. The weak_ptr already
    // makes stale subscriptions inert; this only keeps the maps honest for a
    // session the registry keeps alive past its end.
    void Unsubscribe(const std::string& session_id);

    // The system-prompt section for one session's subscriptions: each
    // channel's instructions (as far as they have been captured), prefixed
    // with the channel name. Empty when no subscribed channel has said
    // anything. A session that subscribed to a channel that has not CONNECTED
    // yet gets no instructions from it -- its first signal is still
    // self-describing via the <signal> wrapper; the section is what gives the
    // model the operator-reviewed framing as early as possible.
    std::string SignalContextFor(const std::vector<std::string>& selectors) const;

    // Testing seam: the number of distinct session ids with a subs_ entry
    // (subscribed or not -- an emptied-but-not-erased entry still counts).
    // Production code never inspects this; it exists to catch a session
    // whose entry outlives normal termination, which grows this map for the
    // life of the process regardless of whether the session ever
    // resubscribes.
    std::size_t SubscriptionEntriesForTesting() const {
        std::lock_guard<std::mutex> lock(mu_);
        return subs_.size();
    }

private:
    struct Subscription {
        std::string channel;
        std::string signal;  // empty = channel-level (extended sources, "sessions")
        std::weak_ptr<Session> session;
    };

    // EXTERNAL delivery: collect live matching sessions under the lock,
    // deliver outside it. `filter_signal` empty matches every subscription of
    // the channel (extended sources are subscribed channel-level).
    Delivery Deliver(const std::string& channel, const std::string& filter_signal,
                     const std::string& wrapped_content);

    mutable std::mutex mu_;
    std::map<std::string, config::ChannelEntry> channels_;  // name -> entry
    std::map<std::string, std::string> instructions_;       // channel -> instructions
    // session_id -> that session's subscriptions. Replacing a session's
    // subscriptions (re-subscribe on resume) overwrites wholesale: a resume
    // is a new stream's word on what it wants, not an accumulation.
    std::map<std::string, std::vector<Subscription>> subs_;
};

// The <signal> wrapper around delivered content -- the provenance frame that
// tells the model where a message came from and that its text is UNTRUSTED
// (external signal content is attacker-adjacent in every mode: the allowlist
// gates shape, never content). Attribute keys outside [a-z0-9._-] or
// colliding with a reserved provenance name (source/signal/from) are silently
// dropped and values are escaped (backslash, quote, control chars), so one
// meta entry cannot break out of its attribute or masquerade as the frame's
// own provenance; values are otherwise verbatim. The body is escaped too
// ('<' -> "&lt;"), so content cannot close the tag early and forge a nested
// <signal> frame. Exposed for the router's tests and the service facade.
std::string WrapSignal(const std::string& source, const std::string& signal,
                       const std::map<std::string, std::string>& attributes,
                       const std::string& content);

}  // namespace minimal::agent
