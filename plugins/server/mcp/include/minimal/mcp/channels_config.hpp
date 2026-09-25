#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "minimal/agent/signal_service.hpp"

namespace minimal::config {

// One channels.json entry: an MCP stdio server that PUSHES signals into
// sessions (a "signal channel"), unlike an mcps.json entry whose tools the
// model PULLS. The trust class differs -- a channel's content lands in front
// of the model unbidden -- which is why channels live in their own file with
// their own mode knob, never mixed into mcps.json.
//
// Shape (top-level object, keyed by channel name):
//
//   {
//     "ci":       { "mode": "restricted", "signals": ["build.completed",
//                                                     "build.failed"],
//                   "command": "bun", "args": ["./ci-channel.ts"], "env": {} },
//     "reminder": { "mode": "extended", "command": "./reminder-channel" }
//   }
//
// mode, default "restricted":
//   - restricted: only the DECLARED signal names may be delivered. Sessions
//     subscribe per signal ("ci/build.completed") and a session-start
//     selector naming an undeclared signal is refused. Catches config drift
//     (the channel was updated to emit new signals nobody reviewed); it does
//     NOT vet content, which is untrusted text in both modes.
//   - extended: any signal name passes. Sessions subscribe at CHANNEL level
//     ("reminder"); per-signal selectors there would be theater.
//
// v1 carries stdio entries only -- there is no transport field. A channel
// needs a process the harness spawns and a persistent inbound path, which the
// hand-rolled streamable-http transport (strictly request/response) cannot
// offer; if http channels ever arrive, the field slots in beside mode.
struct ChannelEntry {
    std::string name;
    // "restricted" unless the file says otherwise: the safe mode is the easy
    // mode. An extended channel is an operator's explicit decision.
    bool extended = false;
    std::vector<std::string> signals;  // restricted only; extended must not set it
    std::string command;
    std::vector<std::string> args;
    std::unordered_map<std::string, std::string> env;
    bool enabled = true;  // same rule as mcps.json: authoring is intent, disabling is opt-out
};

struct ChannelsConfig {
    std::vector<ChannelEntry> entries;
    // Per-entry structural problems, keyed by channel name (the JSON key IS
    // the name, so the label never falls back to a position). A bad entry
    // degrades to a recorded reason rather than taking the good ones down --
    // the McpsConfig shape.
    std::vector<std::string> errors;

    const ChannelEntry* Find(const std::string& name) const;
};

// Pure. Throws only on a file-level problem: a top level that is not an
// object. Per-entry problems land in `errors`, never as exceptions.
ChannelsConfig ParseChannelsConfig(const nlohmann::json& doc);

// Thin: reads the file and parses it. A MISSING file is not an error -- most
// installs configure no channels at all, exactly like mcps.json.
ChannelsConfig LoadChannelsConfig(const std::filesystem::path& path);

// The signal system's name rule and reserved internal source, owned by core
// (agent/signal_service.hpp) -- channels.json validates against the same
// rule the router enforces.
using agent::IsValidSignalName;
inline constexpr std::string_view kReservedInternalChannelName = agent::kInternalSignalSource;

}  // namespace minimal::config
