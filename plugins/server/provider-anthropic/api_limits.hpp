#pragma once

// Facts of the Anthropic API that more than one part of this plugin relies
// on (judgment calls live in policy.hpp).

namespace minimal::provider_anthropic {

// NOLINTBEGIN(readability-magic-numbers) -- documented API limits.
// The smallest thinking budget the API accepts (budget_tokens).
inline constexpr int kMinThinkingBudgetTokens = 1024;
// GET /v1/models' largest page (limit).
inline constexpr int kModelsPageLimit = 1000;
// NOLINTEND(readability-magic-numbers)

}  // namespace minimal::provider_anthropic
