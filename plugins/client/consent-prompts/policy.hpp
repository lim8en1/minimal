#pragma once

// consent-prompts policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::client::consent_prompts {

// The confirm modal is narrower than the kit's default: a short question and
// a row of answers read better without a wide empty frame.
inline constexpr int kConfirmModalMaxWidth = 72;

}  // namespace minimal::client::consent_prompts
