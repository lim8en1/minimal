#pragma once

// SDK policy: the judgment calls the plugin-facing contracts themselves make
// -- defaults an interface states, and limits core and plugins must agree
// on. Every value states why it is what it is (ARCHITECTURE.md "Policy
// constants"). A plugin's own calls live in the plugin's own policy.hpp;
// core's in minimal/client/policy.hpp, which includes this one.

namespace minimal::client {

// Where a status segment ranks when the bar overflows (lowest gives way
// first) unless it declares its own priority.
inline constexpr int kPluginStatusSegmentPriority = 30;

}  // namespace minimal::client
