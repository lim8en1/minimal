#pragma once

// cost-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

namespace minimal::client::cost_view {

// Below a cent the cost shows four decimals: "$0.0042" rather than a "$0.00"
// that reads as free.
inline constexpr double kSubCentUsd = 0.01;

// The cost segment's place in the status bar's give-way order: above the
// plugin default -- spend is decision-relevant -- below live core state.
inline constexpr int kStatusSegmentPriority = 40;

}  // namespace minimal::client::cost_view
