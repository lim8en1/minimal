#pragma once

// config-view policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>

namespace minimal::config_view {

// How often an open form asks for a running config check's findings: often
// enough that a result appears about when it lands, rare enough that a
// 20-second check costs a handful of describes.
inline constexpr std::chrono::milliseconds kCheckPoll{1000};

}  // namespace minimal::config_view
