#pragma once

// session-status policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

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

}  // namespace minimal::session_status
