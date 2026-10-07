#pragma once

// session-status policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstdint>

namespace minimal::session_status {

// Which segment gives way first when the bar overflows: lowest first. The
// live, decision-relevant state (mode, activity, model, context, a paused
// session) outranks reference detail the user can get elsewhere (the tab
// bar's own session name, the version); a plugin segment that declares
// nothing sits at kPluginStatusSegmentPriority (30), between the two.
inline constexpr int kPermissionModePriority = 100;
inline constexpr int kActivityPriority = 90;
inline constexpr int kModelPriority = 80;
inline constexpr int kTokensPriority = 70;
inline constexpr int kAgentPriority = 60;
inline constexpr int kPausedPriority = 50;
inline constexpr int kSessionPriority = 20;
inline constexpr int kVersionPriority = 10;

// Where the segments sit (placement(), ascending within a side). Left, what
// is happening: activity, then the context it fills. Right, what it runs
// on: model, agent, mode, and a paused session last, where the eye lands at
// the line's end. Spaced by 10 so another plugin's segment can sit between
// (cost-view's cost lands at left 30, right after tokens). {version} and
// {session} place nowhere: the banner and the tab bar carry them.
inline constexpr int kActivityOrder = 10;
inline constexpr int kTokensOrder = 20;
inline constexpr int kModelOrder = 10;
inline constexpr int kAgentOrder = 20;
inline constexpr int kPermissionModeOrder = 30;
inline constexpr int kPausedOrder = 40;

// Context occupancy that tints the token segment: attention as compaction
// nears, error when the window is all but full.
inline constexpr std::int64_t kContextAttentionPercent = 80;
inline constexpr std::int64_t kContextErrorPercent = 95;

}  // namespace minimal::session_status
