#pragma once

// startup policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::startup {

// A pattern-rule list longer than this draws a sanity warning: resolve cost
// grows with the list, and a list this long is almost always a mis-paste.
inline constexpr std::size_t kPatternRuleCountWarning = 200;

// settings.json max_concurrent_subagents above this draws a warning, never a
// refusal: past it the task has outgrown the harness (ARCHITECTURE.md,
// "Subagents"), but that is the operator's call.
inline constexpr int kSubagentLimitWarning = 8;

}  // namespace minimal::startup
