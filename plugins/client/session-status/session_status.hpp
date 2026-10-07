#pragma once

// session-status -- the status bar's usual segments: {version}, {session},
// {tokens}, {model}, {agent}, {permission_mode}, {paused}, {activity}. Each
// renders from the focused tab's state the host hands every segment
// (plugins::TabStatus); the bar's layout and format string are core's.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "minimal/client/sdk/registries.hpp"

namespace minimal::session_status {

namespace plugins = minimal::client::plugins;

// "62k/200k · 31%" (window known) or "4.2k tok" (not). Compact on purpose --
// the exact count was seven digits of noise at a glance -- but never
// truncated: every figure shown is the real one, rounded.
std::string FormatTokenUsage(std::int64_t used, std::int64_t max);

// The {permission_mode} value. A shift+tab sent but not yet confirmed by the
// server's metadata event is the tab's DESIRED state: while it differs from
// the applied mode -- and the tab has a session to apply into -- the desired
// value renders with the "→ " pending marker, the convention {model}/{agent}
// follow. Empty `desired` renders `applied`; both empty render "" (the
// segment renders nothing).
std::string PermissionModeSegmentValue(const std::string& applied, const std::string& desired,
                                       bool has_session);

// {activity} with the live-view cut notice: when the live-segment cap
// dropped older rows of this turn, the suffix says so, appended with the
// " · " the bar renders between segments; over an empty activity it stands
// alone. Composed, never overriding: the state word must survive.
std::string WithHiddenRowsSuffix(std::string activity);

// Every segment this plugin registers.
std::vector<std::unique_ptr<plugins::StatusSegment>> MakeSegments();

}  // namespace minimal::session_status
