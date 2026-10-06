#pragma once

// tools-fs policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::tools_fs {

// read: lines per call without a limit, the hard ceiling on any window, and
// the cut for one long line. A screenful of a large file many times over,
// while a minified bundle or a log cannot flood the context in one call.
inline constexpr int kDefaultReadLimitLines = 2000;
inline constexpr int kDefaultReadMaxLines = 2000;
inline constexpr int kDefaultReadMaxLineLength = 2000;

// edit: the largest file edited in place. Source files are far smaller; a
// file past this is generated or data, and a string replace over it is a
// mistake waiting to be written back.
inline constexpr std::int64_t kDefaultEditMaxFileBytes = 5 * static_cast<std::int64_t>(common::kMiB);

// The diff shown after an edit: the alignment search gives up past this
// distance (Myers keeps about D^2 ints, a few MB here, and a change that large
// is not reviewable as a diff anyway), and a longer hunk is summarized.
inline constexpr int kMaxDiffEditDistance = 1000;
inline constexpr std::size_t kMaxDiffHunkLines = 2000;

}  // namespace minimal::tools_fs
