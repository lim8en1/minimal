#pragma once

// project-context policy: every value here is a judgment call, not a fact or
// a derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::project_context {

// An AGENTS.md / CLAUDE.md read into every session's context: room for a long
// project guide, a cap on a file that is really a dump.
inline constexpr std::int64_t kDefaultMaxBytes = 256 * static_cast<std::int64_t>(common::kKiB);

}  // namespace minimal::project_context
