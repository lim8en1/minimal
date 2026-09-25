#pragma once

// The signal-channel protocol, shared by the MCP client (which detects the
// capability and carries notifications) and the SignalRouter (which parses
// and routes them). Constants only -- mcp/ must not include agent/ headers
// (agent depends on mcp, never the reverse), and the router needs the same
// strings the manager checks.
//
// The wire contract, v1:
//   - a channel server declares experimental capability "minimal/signal" in
//     its initialize result;
//   - it pushes notifications on "notifications/minimal/signal" with params
//     { signal: string, content: string, meta?: map<string,string> };
//   - `signal` and `meta` KEYS must match [a-z0-9._-] (config::
//     IsValidSignalName): a bad signal name drops the notification with a
//     log line, a bad meta key silently drops that key -- the same leniency
//     split Claude's channels apply to attributes;
//   - no acks, fire-and-forget: delivery to a session with no matching
//     subscription is a logged drop, never an error back to the server.
// `content` is untrusted text in every mode; the router wraps it in a
// <signal> tag so the model can tell source and provenance.

#include <map>
#include <string>
#include <string_view>

#include "minimal/mcp/channels_config.hpp"

namespace minimal::mcp {

inline constexpr const char* kSignalCapability = "minimal/signal";
inline constexpr const char* kSignalNotificationMethod = "notifications/minimal/signal";

// The builtin INTERNAL source: cross-session messages (the message_session
// tool, future plugin producers via core.signals). Never an MCP channel, never
// in channels.json, no mode -- its trust story is sender-gated by the tool
// permission model and receiver-gated by subscription, and that is the whole
// gate. A channel in channels.json named this is refused (sanity check).
// Defined in config/ (the layer that owns the reserved-name rule); the alias
// keeps the protocol vocabulary in one place for mcp/ and agent/ readers.
inline constexpr std::string_view kInternalSignalSource =
    minimal::config::kReservedInternalChannelName;

// One decoded notifications/minimal/signal frame: what the router's
// DeliverExternal takes. `meta` keeps only string values; key filtering (the
// charset, the reserved provenance names) is the router's job.
struct SignalFrame {
    std::string signal;
    std::string content;
    std::map<std::string, std::string> meta;
};

// Decodes one JSON-RPC line a channel server wrote. True with `frame` filled
// for a signal notification. False otherwise: `error` stays EMPTY for a frame
// that is simply not a signal (another MCP notification -- the server's own
// business, ignored silently) and says why for a malformed one (not JSON, no
// params, missing string signal/content), which the caller logs and drops.
// Never throws.
bool ParseSignalNotification(const std::string& raw, SignalFrame& frame, std::string& error);

}  // namespace minimal::mcp
