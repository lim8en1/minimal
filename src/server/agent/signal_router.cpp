#include "minimal/agent/signal_router.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/agent/session.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/llm/message.hpp"
#include "minimal/mcp/signal_protocol.hpp"

namespace minimal::agent {

namespace {

// Attribute keys are the same charset as signal names (config::
// IsValidSignalName): a key outside it is silently dropped rather than
// rejected -- a meta entry is decoration, and one odd key must not cost the
// whole notification. Mirrors the wire rule for params.meta.
std::map<std::string, std::string> SanitizeMeta(const nlohmann::json& meta) {
    std::map<std::string, std::string> out;
    if (!meta.is_object()) return out;
    for (const auto& [key, value] : meta.items()) {
        if (!config::IsValidSignalName(key) || !value.is_string()) continue;
        out.emplace(key, value.get<std::string>());
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
        else if (static_cast<unsigned char>(c) < 0x20) continue;  // control chars: dropped
        else out += c;
    }
    return out;
}

}  // namespace

std::string WrapSignal(const std::string& source, const std::string& signal,
                       const std::map<std::string, std::string>& attributes,
                       const std::string& content) {
    std::string tag = "<signal source=\"" + source + "\"";
    if (!signal.empty()) tag += " signal=\"" + signal + "\"";
    for (const auto& [key, value] : attributes) {
        // The same charset rule as the wire params: a key outside it is
        // silently dropped rather than rejected -- one odd attribute must not
        // cost the whole signal.
        if (!config::IsValidSignalName(key)) continue;
        tag += " " + key + "=\"" + EscapeAttrValue(value) + "\"";
    }
    tag += ">";
    if (content.empty()) return tag + "</signal>";
    return tag + "\n" + content + "\n</signal>";
}

void SignalRouter::RegisterChannel(const config::ChannelEntry& entry) {
    std::lock_guard<std::mutex> lock(mu_);
    channels_[entry.name] = entry;
}

void SignalRouter::SetChannelInstructions(const std::string& channel,
                                          const std::string& instructions) {
    std::lock_guard<std::mutex> lock(mu_);
    if (channels_.count(channel) == 0) return;  // unknown source: store nothing
    instructions_[channel] = instructions;
}

void SignalRouter::OnMcpNotification(const std::string& channel, const std::string& raw) {
    // Parse leniently: a malformed line from a channel server is a logged
    // drop, never an exception into the read thread that carries it.
    const nlohmann::json message = nlohmann::json::parse(raw, nullptr, /*allow_exceptions=*/false);
    if (!message.is_object()) {
        minimal::common::DefaultLogger().warn("signal channel '{}': dropped a non-JSON frame", channel);
        return;
    }
    if (message.value("method", std::string()) != mcp::kSignalNotificationMethod) {
        return;  // some other MCP notification: the server's own business
    }
    const auto params = message.find("params");
    if (params == message.end() || !params->is_object()) {
        minimal::common::DefaultLogger().warn("signal channel '{}': dropped a signal frame without params",
                             channel);
        return;
    }

    const auto signal_it = params->find("signal");
    const auto content_it = params->find("content");
    if (signal_it == params->end() || !signal_it->is_string() || content_it == params->end() ||
        !content_it->is_string()) {
        minimal::common::DefaultLogger().warn("signal channel '{}': dropped a signal frame missing string "
                             "'signal'/'content'",
                             channel);
        return;
    }
    const std::string signal = signal_it->get<std::string>();
    if (!config::IsValidSignalName(signal)) {
        // A bad NAME is a contract violation worth one line per drop (config
        // drift or a misbehaving server), unlike a bad meta key above.
        minimal::common::DefaultLogger().warn("signal channel '{}': dropped a signal with invalid name '{}'",
                             channel, signal);
        return;
    }
    const std::string content = content_it->get<std::string>();
    const auto meta_it = params->find("meta");

    // Restricted allowlist check: the declared signals are what the operator
    // reviewed. A name outside the list is exactly the config-drift case this
    // mode exists to catch, so it is loud.
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = channels_.find(channel);
        if (it == channels_.end()) {
            minimal::common::DefaultLogger().warn("signal channel '{}': not configured (channels.json); dropping",
                                 channel);
            return;
        }
        if (!it->second.extended &&
            std::find(it->second.signals.begin(), it->second.signals.end(), signal) ==
                it->second.signals.end()) {
            minimal::common::DefaultLogger().warn(
                "signal channel '{}': signal '{}' is not in the declared signals list; dropping",
                channel, signal);
            return;
        }
    }

    const std::map<std::string, std::string> meta =
        meta_it == params->end() ? std::map<std::string, std::string>{} : SanitizeMeta(*meta_it);
    const std::string wrapped = WrapSignal(channel, signal, meta, content);
    Deliver(channel, signal, wrapped);
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
            if (sub.channel != mcp::kInternalSignalSource) continue;
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

    const std::string wrapped = WrapSignal(std::string(mcp::kInternalSignalSource),
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
                                     const std::string& wrapped_content) {
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
        if (session->QueueInjectedMessage(std::move(message))) any = true;
    }
    return any ? Delivery::kDelivered : Delivery::kDropped;
}

bool SignalRouter::CheckSelector(const std::string& selector, std::string& error,
                                 std::string* channel) const {
    // Split on the FIRST '/': "ci/build.completed" is channel "ci", signal
    // "build.completed". The charset rule guarantees a signal never contains
    // '/', so the split is unambiguous -- and a channel part of "a/b" was
    // already impossible (channels.json validated it).
    const auto slash = selector.find('/');
    const std::string channel_part =
        slash == std::string::npos ? selector : selector.substr(0, slash);
    const std::string signal_part =
        slash == std::string::npos ? std::string() : selector.substr(slash + 1);
    if (channel_part.empty() ||
        (!signal_part.empty() && !config::IsValidSignalName(signal_part))) {
        error = "invalid signal selector '" + selector +
                "' (expected 'channel' or 'channel/signal', names in [a-z0-9._-])";
        return false;
    }
    if (channel != nullptr) *channel = channel_part;

    const bool internal = channel_part == mcp::kInternalSignalSource;
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
        error = "unknown signal channel '" + channel_part + "' (channels.json declares none)";
        return false;
    }
    if (it->second.extended) {
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
                    (it->second.signals.empty() ? std::string() : it->second.signals.front()) +
                    "')";
            return false;
        }
        if (std::find(it->second.signals.begin(), it->second.signals.end(), signal_part) ==
            it->second.signals.end()) {
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
