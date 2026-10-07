#pragma once

// checklist-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

namespace minimal::checklist_view {

// A failing list call is logged on the first failure and every this many
// after: visible in the log, not a line per poll.
inline constexpr int kLogEveryNthListFailure = 10;

// {todo}'s place in the status bar: the left side's end, after what is
// happening and what it costs (cost-view sits at left 30-40) -- progress
// through the plan reads with the activity.
inline constexpr int kTodoOrder = 50;

}  // namespace minimal::checklist_view
