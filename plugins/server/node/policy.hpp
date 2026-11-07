#pragma once

// node policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::node {

// Violations listed back to the model (a rejected submit_result) or the
// client (a rejected input). Enough to fix a whole object in one retry; past
// it a count stands in, so a wildly wrong value cannot flood the context.
inline constexpr std::size_t kMaxReportedViolations = 20;

// Meta-schema violations quoted when a preset's contract is not a valid
// schema: the operator fixes a typo, and the first few name it.
inline constexpr std::size_t kMaxReportedSchemaErrors = 5;

}  // namespace minimal::node
