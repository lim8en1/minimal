#pragma once

// decision-advisor policy: every value here is a judgment call, not a fact or
// a derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::decision_advisor {

// How much of a judge's error body an abstain reason quotes: enough to see
// what went wrong in a log line.
inline constexpr std::size_t kMaxErrorDetailBytes = 200;

}  // namespace minimal::decision_advisor
