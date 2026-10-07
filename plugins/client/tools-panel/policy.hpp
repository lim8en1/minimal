#pragma once

// tools-panel policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::tools_panel {

// Trailing output lines shown per tool: enough to tell a progressing command
// from a stuck one; the transcript keeps the rest. (config: "output_lines")
inline constexpr int kDefaultOutputLines = 100;
// The session column's width, so summaries line up across tabs.
inline constexpr std::size_t kSessionColumn = 16;

}  // namespace minimal::tools_panel
