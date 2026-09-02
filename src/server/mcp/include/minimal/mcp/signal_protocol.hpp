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

#include <string_view>

#include "minimal/config/channels_config.hpp"

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

}  // namespace minimal::mcp
