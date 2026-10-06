#pragma once

// provider-anthropic policy: every value here is a judgment call, not a fact
// or a derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <string_view>
#include <utility>

namespace minimal::provider_anthropic {

// The effort ladder, lowest first (the order step-down walks), with the
// thinking budget each rung gets on models that only take a budget
// (pre-adaptive: Haiku 4.5 and older). A scale, not data: each rung doubles
// the last, all well under every such model's output cap (the caller clamps
// against max_tokens when known). "minimal" has no rung: no thinking block.
inline constexpr std::pair<std::string_view, int> kThinkingBudgets[] = {
    {"low", 4096}, {"medium", 8192}, {"high", 16384}, {"xhigh", 32768}, {"max", 65536},
};

// Output kept for the visible answer when a thinking budget is clamped under
// max_tokens: a thinking-heavy turn still has room to say something.
inline constexpr int kReservedVisibleOutputTokens = 1024;

}  // namespace minimal::provider_anthropic
