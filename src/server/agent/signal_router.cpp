#include "minimal/agent/signal_router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utility>

#include "minimal/agent/session.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/message.hpp"

namespace minimal::agent {

namespace {

// Attribute names WrapSignal itself always emits (source, signal) or that
// DeliverInternal's own call adds (from). A channel's meta map must never be
// able to supply one of these: SanitizeMeta runs only on EXTERNAL meta (the
// internal path builds its `from` attribute directly, bypassing this
// function), so blocking all three here costs the internal path nothing and
// closes off a channel duplicating/overriding its own provenance attributes.
constexpr std::array<std::string_view, 3> kReservedAttributeKeys = {"source", "signal", "from"};

bool IsReservedAttributeKey(const std::string& key) {
    return std::find(kReservedAttributeKeys.begin(), kReservedAttributeKeys.end(), key) !=
           kReservedAttributeKeys.end();
}

// Attribute keys are the same charset as signal names (config::
// IsValidSignalName): a key outside it is silently dropped rather than
// rejected -- a meta entry is decoration, and one odd key must not cost the
// whole notification. Mirrors the wire rule for params.meta. A key colliding
// with a reserved provenance attribute is dropped the same way: decoration
// must not be able to masquerade as the frame's own source/signal/from.
std::map<std::string, std::string> SanitizeMeta(const std::map<std::string, std::string>& meta) {
    std::map<std::string, std::string> out;
    for (const auto& [key, value] : meta) {
        if (!IsValidSignalName(key)) continue;
        if (IsReservedAttributeKey(key)) continue;
        out.emplace(key, value);
    }
    return out;
}

// The content BODY is untrusted text placed between the wrapper's open and
// close tags. Attribute values are escaped so one meta entry cannot break out
// of its quoted attribute (see EscapeAttrValue) -- but the body sat verbatim,
// so content containing a literal "</signal>" followed by a forged
// "<signal source=\"...\">" produced a second, well-formed frame the model
// could not distinguish from a real one. Escaping every '<' removes the
// ability to open ANY tag from within the body, which is the same guarantee
// the attribute escaping gives attributes.
std::string EscapeContent(const std::string& content) {
    std::string out;
    out.reserve(content.size());
    for (const char c : content) {
        if (c == '<') out += "&lt;";
        else out += c;
    }
    return out;
}

// Attribute VALUES are escaped so one meta entry cannot break out of its
// quoted attribute and masquerade as structure: backslash and quote are
// escaped, control characters (CR/LF among them) are dropped. The VALUE text
// itself is otherwise verbatim -- a session id (uppercase ULID) or anything a
// channel sends; only the KEY is charset-filtered.
std::string EscapeAttrValue(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (std::iscntrl(static_cast<unsigned char>(c))) continue;  // control chars: dropped
        else out += c;
    }
    return out;
}

}  // namespace

std::string WrapSignal(const std::string& source, const std::string& signal,
                       const std::map<std::string, std::string>& attributes,
                       const std::string& content) {
    // Escaped like every other attribute below (EscapeAttrValue), not left
    // verbatim: both call sites today pre-validate source/signal against
    // IsValidSignalName's [a-z0-9._-] charset, so this is currently
    // unreachable in practice, but this function is documented (see the
    // header) as reusable beyond those two callers -- a future caller
    // passing an unvalidated value containing '"' would otherwise break out
    // of the attribute and forge signal structure, exactly the injection
    // class this function exists to prevent for every other field.
    std::string tag = "<signal source=\"" + EscapeAttrValue(source) + "\"";
    if (!signal.empty()) tag += " signal=\"" + EscapeAttrValue(signal) + "\"";
    for (const auto& [key, value] : attributes) {
        // The same charset rule as the wire params: a key outside it is
        // silently dropped rather than rejected -- one odd attribute must not
        // cost the whole signal.
        if (!IsValidSignalName(key)) continue;
        tag += " " + key + "=\"" + EscapeAttrValue(value) + "\"";
    }
    tag += ">";
    if (content.empty()) return tag + "</signal>";
    return tag + "\n" + EscapeContent(content) + "\n</signal>";
}

bool SignalRouter::RegisterExternalChannel(const ExternalChannel& channel,
                                           ExternalChannelSource* source, std::string& error) {
    if (!IsValidSignalName(channel.name)) {
        error = "invalid channel name '" + channel.name + "' (names in [a-z0-9._-])";
        return false;
    }
    if (channel.name == kInternalSignalSource) {
        error = "channel name '" + channel.name + "' is reserved for cross-session messages";
        return false;
    }
    if (!channel.extended && channel.signals.empty()) {
        error = "restricted channel '" + channel.name + "' declares no signals";
        return false;
    }
    if (channel.extended && !channel.signals.empty()) {
        error = "extended channel '" + channel.name +
                "' declares a signals list (extended means every signal passes)";
        return false;
    }
    for (const auto& signal : channel.signals) {
        if (!IsValidSignalName(signal)) {
            error = "channel '" + channel.name + "' declares invalid signal '" + signal + "'";
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!channels_.emplace(channel.name, Channel{channel, source}).second) {
        error = "channel '" + channel.name + "' is already registered";
        return false;
    }
    return true;
}

void SignalRouter::SetChannelInstructions(const std::string& channel,
                                          const std::string& instructions) {
    std::lock_guard<std::mutex> lock(mu_);
    if (channels_.count(channel) == 0) return;  // unknown source: store nothing
    instructions_[channel] = instructions;
}

bool SignalRouter::EnsureChannelAlive(const std::string& channel, std::string& error) {
    if (channel == kInternalSignalSource) return true;  // nothing to bring up
    ExternalChannelSource* source = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = channels_.find(channel);
        if (it == channels_.end()) {
            error = "unknown signal channel '" + channel + "'";
            return false;
        }
        source = it->second.source;
    }
    // Outside the lock: a producer's connect may block for its whole budget,
    // and every delivery on every channel takes mu_.
    return source == nullptr || source->EnsureAlive(channel, error);
}

SignalDelivery SignalRouter::DeliverExternal(const std::string& channel, const std::string& signal,
                                             const std::string& content,
                                             const std::map<std::string, std::string>& meta) {
    if (!IsValidSignalName(signal)) {
        // A bad NAME is a contract violation worth one line per drop (config
        // drift or a misbehaving producer), unlike a bad meta key.
        minimal::common::DefaultLogger().warn("signal channel '{}': dropped a signal with invalid name '{}'",
                             channel, signal);
        return Delivery::kInvalid;
    }

    // The owner's per-session gate, applied by each receiving worker.
    std::optional<InjectionGate> gate;
    // Restricted allowlist check: the declared signals are what the operator
    // reviewed. A name outside the list is exactly the config-drift case this
    // mode exists to catch, so it is loud.
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = channels_.find(channel);
        if (it == channels_.end()) {
            minimal::common::DefaultLogger().warn("signal channel '{}': not registered; dropping",
                                 channel);
            return Delivery::kInvalid;
        }
        const auto& declared = it->second.declaration;
        if (!declared.owner.empty()) {
            gate = InjectionGate{declared.owner,
                                 tools::PermissionSubject{"signal", channel + "/" + signal,
                                                          {{"channel", channel}, {"signal", signal}}},
                                 "signal " + channel + "/" + signal};
        }
        if (!declared.extended &&
            std::find(declared.signals.begin(), declared.signals.end(), signal) ==
                declared.signals.end()) {
            minimal::common::DefaultLogger().warn(
                "signal channel '{}': signal '{}' is not in the declared signals list; dropping",
                channel, signal);
            return Delivery::kInvalid;
        }
    }

    const std::string wrapped = WrapSignal(channel, signal, SanitizeMeta(meta), content);
    // Every other drop path above logs a line; a well-formed, allowlisted
    // signal that never reaches a session must too, or these specific (and
    // likely common) cases are invisible to an operator debugging "why
    // didn't my signal fire". kNoSubscriber and kDropped are different
    // causes -- no matching subscription at all, versus a subscribed
    // session whose InjectionQueue refused every delivery attempt (it is
    // terminating, or already staged kMaxPendingInjections messages) -- so they get
    // distinct messages rather than one that fits neither.
    const Delivery delivery = Deliver(channel, signal, wrapped, gate);
    switch (delivery) {
        case Delivery::kNoSubscriber:
            minimal::common::DefaultLogger().warn(
                "signal channel '{}': signal '{}' has no subscriber; dropped", channel, signal);
            break;
        case Delivery::kDropped:
            minimal::common::DefaultLogger().warn(
                "signal channel '{}': signal '{}' had a subscriber but delivery was refused "
                "(terminating, or its injection queue is full); dropped",
                channel, signal);
            break;
        case Delivery::kDelivered:
        case Delivery::kInvalid:
            break;
    }
    return delivery;
}

SignalDelivery SignalRouter::DeliverInternal(const std::string& from_session,
                                             const std::string& target_session,
                                             const std::string& content) {
    if (from_session.empty() || target_session.empty() || content.empty()) {
        return Delivery::kInvalid;
    }
    // Self-address refused at the router as a second guard behind the tool's
    // own check: the internal source is for A -> B exchange, and a message
    // that loops back into the sender's own queue is exactly the feedback
    // loop the refusal exists to prevent.
    if (target_session == from_session) return Delivery::kInvalid;

    // ADDRESSED, not broadcast: the target's own subscription decides. The
    // weak_ptr both proves liveness (terminated sessions lock to null or
    // refuse via terminated_) and keeps the router out of session lifetime.
    std::shared_ptr<Session> target;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = subs_.find(target_session);
        if (it == subs_.end()) return Delivery::kNoSubscriber;
        for (auto& sub : it->second) {
            if (sub.channel != kInternalSignalSource) continue;
            if (auto session = sub.session.lock()) {
                target = std::move(session);
                break;
            }
        }
        it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                        [](const Subscription& s) {
                                            return s.session.expired();
                                        }),
                         it->second.end());
    }
    if (target == nullptr) return Delivery::kNoSubscriber;

    const std::string wrapped = WrapSignal(std::string(kInternalSignalSource),
                                           /*signal=*/std::string(), {{"from", from_session}},
                                           content);
    // The message lands as a USER turn: injected signals are the same shape
    // as a non-blocking subagent's completion (the pattern subagent_manager
    // established), and the seedless wake path treats the injected messages
    // as that turn's user content.
    llm::Message message{llm::Role::User, {llm::TextBlock{wrapped}}};
    return target->QueueInjectedMessage(std::move(message)) ? Delivery::kDelivered
                                                            : Delivery::kDropped;
}

SignalDelivery SignalRouter::Deliver(const std::string& channel,
                                     const std::string& filter_signal,
                                     const std::string& wrapped_content,
                                     const std::optional<InjectionGate>& gate) {
    // Collect under the lock, inject outside it: QueueInjectedMessage takes
    // the session's own mutex and notifies its worker -- held under mu_ that
    // would invert against nothing today but would be one config edit away
    // from a lock-order surprise, and the deliver path runs on a channel's
    // read thread where blocking costs every later frame.
    std::vector<std::shared_ptr<Session>> targets;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [session_id, subs] : subs_) {
            // Prune first, then match: a stale entry (the session was torn
            // down and the registry let it go) is inert via the weak_ptr but
            // should not accumulate. Pruning invalidates iterators, so the
            // match scan runs after the sweep, on the settled vector.
            subs.erase(std::remove_if(subs.begin(), subs.end(),
                                      [](const Subscription& s) { return s.session.expired(); }),
                       subs.end());
            for (auto& sub : subs) {
                if (sub.channel != channel) continue;
                // Restricted subscriptions name ONE signal; extended and
                // internal subscriptions are channel-level (empty sub.signal
                // matches everything the source emits).
                if (!filter_signal.empty() && !sub.signal.empty() && sub.signal != filter_signal) {
                    continue;
                }
                if (auto session = sub.session.lock()) targets.push_back(std::move(session));
            }
        }
    }

    if (targets.empty()) return Delivery::kNoSubscriber;

    bool any = false;
    for (auto& session : targets) {
        // The message lands as a USER turn: injected signals are the same
        // shape as a non-blocking subagent's completion (the pattern
        // subagent_manager.cpp established), and the seedless wake path treats
        // the injected messages as that turn's user content.
        llm::Message message{llm::Role::User, {llm::TextBlock{wrapped_content}}};
        if (session->QueueInjectedMessage(std::move(message), gate)) any = true;
    }
    return any ? Delivery::kDelivered : Delivery::kDropped;
}

bool SignalRouter::CheckSelector(const std::string& selector, std::string& error,
                                 std::string* channel) const {
    // Split on the FIRST '/': "ci/build.completed" is channel "ci", signal
    // "build.completed". The charset rule guarantees a signal never contains
    // '/', so the split is unambiguous -- and a channel part of "a/b" was
    // already impossible (RegisterExternalChannel validated it).
    const auto slash = selector.find('/');
    const std::string channel_part =
        slash == std::string::npos ? selector : selector.substr(0, slash);
    const std::string signal_part =
        slash == std::string::npos ? std::string() : selector.substr(slash + 1);
    if (channel_part.empty() ||
        (!signal_part.empty() && !IsValidSignalName(signal_part))) {
        error = "invalid signal selector '" + selector +
                "' (expected 'channel' or 'channel/signal', names in [a-z0-9._-])";
        return false;
    }
    if (channel != nullptr) *channel = channel_part;

    const bool internal = channel_part == kInternalSignalSource;
    if (internal) {
        if (!signal_part.empty()) {
            error = "the internal source subscribes at source level ('sessions')";
            return false;
        }
        return true;
    }

    std::lock_guard<std::mutex> lock(mu_);
    const auto it = channels_.find(channel_part);
    if (it == channels_.end()) {
        error = "unknown signal channel '" + channel_part + "' (no such channel is registered)";
        return false;
    }
    const ExternalChannel& declared = it->second.declaration;
    if (declared.extended) {
        if (!signal_part.empty()) {
            error = "channel '" + channel_part +
                    "' is extended: subscribe at channel level ('" + channel_part +
                    "'), not per signal -- extended means every signal passes";
            return false;
        }
    } else {
        if (signal_part.empty()) {
            error = "channel '" + channel_part +
                    "' is restricted: subscribe per signal (e.g. '" + channel_part + "/" +
                    (declared.signals.empty() ? std::string() : declared.signals.front()) +
                    "')";
            return false;
        }
        if (std::find(declared.signals.begin(), declared.signals.end(), signal_part) ==
            declared.signals.end()) {
            error = "signal '" + signal_part + "' is not declared by channel '" + channel_part +
                    "'";
            return false;
        }
    }
    return true;
}

bool SignalRouter::Subscribe(std::shared_ptr<Session> session, const std::string& selector,
                             std::string& error) {
    if (session == nullptr) {
        error = "no session to subscribe";
        return false;
    }
    if (!CheckSelector(selector, error)) return false;

    const auto slash = selector.find('/');
    const std::string channel =
        slash == std::string::npos ? selector : selector.substr(0, slash);
    const std::string signal =
        slash == std::string::npos ? std::string() : selector.substr(slash + 1);

    std::lock_guard<std::mutex> lock(mu_);
    auto& subs = subs_[session->session_id()];
    subs.erase(std::remove_if(subs.begin(), subs.end(),
                              [&channel, &signal](const Subscription& s) {
                                  return s.channel == channel && s.signal == signal;
                              }),
               subs.end());
    subs.push_back(Subscription{channel, signal, std::move(session)});
    return true;
}

void SignalRouter::Unsubscribe(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(mu_);
    subs_.erase(session_id);
}

std::string SignalRouter::SignalContextFor(const std::vector<std::string>& selectors) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string out;
    for (const auto& selector : selectors) {
        const auto slash = selector.find('/');
        const std::string channel =
            slash == std::string::npos ? selector : selector.substr(0, slash);
        const auto instructions = instructions_.find(channel);
        if (instructions == instructions_.end() || instructions->second.empty()) continue;
        if (!out.empty()) out += "\n\n";
        out += "Channel '" + channel + "': " + instructions->second;
    }
    return out;
}

}  // namespace minimal::agent
