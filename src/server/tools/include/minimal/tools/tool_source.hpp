#pragma once

// Plugin-owned DYNAMIC tool sources: a plugin whose tools are not a fixed set
// known at registration (the MCP plugin: one tool group per configured
// server, discovered when the server comes up) offers its members here, and a
// preset allows them by name:
//
//   tool_sources:
//     mcp: [github, search]
//
// Core owns the per-session mechanics -- which members a preset allows, the
// subagent intersection (a child never gets a member its parent lacks),
// activating them at session start and on every /agent switch, and exposing
// exactly the active groups' tools to the model. The source owns everything
// about its members: configuration, connections, liveness, the tools they
// publish (ToolRegistry::ReplaceGroupTools, inside a namespace the plugin
// reserved), and the permission model governing those tools.

#include <memory>
#include <string>
#include <vector>

#include "minimal/plugins/attributed_registry.hpp"

namespace minimal::tools {

class ToolSource {
public:
    virtual ~ToolSource() = default;

    // Every member the operator configured (enabled or not). The startup
    // sanity check warns about a preset naming anything else.
    virtual std::vector<std::string> Members() const = 0;

    // The ToolRegistry group `member`'s tools are published under. A session
    // whose preset allows the member keeps this group active whether or not
    // the member is up right now: the group is empty until it is, and fills
    // the moment the source publishes into it (a late connect, a re-enable)
    // -- no restart, no preset switch.
    virtual std::string GroupOf(const std::string& member) const = 0;

    // Brings `member` up for a session whose preset allows it -- false with
    // `error` set when it cannot come up. A member that is deliberately off
    // (an operator toggle) is not a failure: answer true and publish nothing
    // until it is turned on. Called on the session worker at session start
    // and preset switch, and on the spawner's thread for a subagent; may
    // block for the source's own connect budget. Must not throw (a throw is
    // treated as an unavailable member).
    virtual bool Activate(const std::string& member, std::string& error) = 0;

    // What the operator can do about `member` when it did not come up ("retry
    // it with /mcp github enable"), appended to the failure message. Empty:
    // no hint.
    virtual std::string ReconnectHint(const std::string& member) const {
        (void)member;
        return {};
    }
};

// Keyed by source name -- the key a preset's tool_sources map uses. One
// implementation per name; a second registration is a collision the sanity
// check reports as fatal.
class ToolSourceRegistry : public plugins::AttributedRegistry<std::shared_ptr<ToolSource>> {
public:
    std::shared_ptr<ToolSource> Find(std::string_view name) const {
        const auto* entry = FindEntry(name);
        return entry != nullptr ? *entry : nullptr;
    }
};

}  // namespace minimal::tools
