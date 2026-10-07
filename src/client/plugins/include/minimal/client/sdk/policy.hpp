#pragma once

// SDK policy: the judgment calls the plugin-facing contracts themselves make
// -- defaults an interface states, and limits core and plugins must agree
// on. Every value states why it is what it is (ARCHITECTURE.md "Policy
// constants"). A plugin's own calls live in the plugin's own policy.hpp;
// core's in minimal/client/policy.hpp, which includes this one.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace minimal::client {

// Where a status segment ranks when the bar overflows (lowest gives way
// first) unless it declares its own priority.
inline constexpr int kPluginStatusSegmentPriority = 30;

// Sessions per ListSessions page (pickers, `-l`, subagent scans).
inline constexpr std::int32_t kSessionListPageSize = 200;

// A tool call's input preview on its row (core summarizes, renderers draw).
inline constexpr std::size_t kToolInputSummaryBytes = 120;

// Picker columns: a session/preset name.
inline constexpr std::size_t kNameColumnCap = 40;

// The status bar's session-name column.
inline constexpr std::size_t kStatusSessionColumnCap = 24;

// A plugin view's data feed (checklist, cost): after its push subscription
// fails it marks the cache stale and polls every poll interval for the
// fallback period before subscribing again; consecutive failures double the
// period up to the ceiling, so a server without the plugin is not hammered.
inline constexpr std::chrono::milliseconds kPluginFeedPollInterval = std::chrono::seconds(3);
inline constexpr std::chrono::milliseconds kPluginFeedFallbackPeriod = std::chrono::seconds(30);
inline constexpr std::chrono::milliseconds kPluginFeedMaxFallbackPeriod = std::chrono::minutes(5);

}  // namespace minimal::client
