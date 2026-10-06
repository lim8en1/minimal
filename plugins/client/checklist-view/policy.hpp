#pragma once

// checklist-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

namespace minimal::checklist_view {

// A failing list call is logged on the first failure and every this many
// after: visible in the log, not a line per poll.
inline constexpr int kLogEveryNthListFailure = 10;

}  // namespace minimal::checklist_view
