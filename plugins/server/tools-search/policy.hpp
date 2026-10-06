#pragma once

// tools-search policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::tools_search {

// --- plugins.json tools-search.config defaults -----------------------------------------

// grep: results shown without a head_limit, and the ceiling on collected
// results -- a screenful of matches by default, a few pages at most.
inline constexpr int kDefaultGrepHeadLimit = 250;
inline constexpr int kDefaultGrepMaxResults = 1000;
// Files larger than this are skipped: source files are far smaller; a big file
// is data or generated, and searching it costs more than it tells.
inline constexpr std::int64_t kDefaultGrepMaxFileBytes = static_cast<std::int64_t>(common::kMiB);
// Context lines per side, and the cut for one long line in the output.
inline constexpr int kDefaultGrepMaxContextLines = 10;
inline constexpr int kDefaultGrepMaxLineLength = 2000;
// glob: rows shown by default, and the ceiling on rows one call returns.
inline constexpr int kDefaultGlobLimit = 200;
inline constexpr int kDefaultGlobMaxResults = 2000;
// Wall-clock bound on one walk (grep or glob): a session worker blocks its
// session, so a huge tree yields partial results in seconds, never a hang.
inline constexpr std::chrono::milliseconds kDefaultWalkTimeout = std::chrono::seconds(10);

// --- walk and pattern bounds ------------------------------------------------------------

// The walk's allocation ceilings (disclosed in WalkStats, never silent).
inline constexpr int kMaxWalkDepth = 1024;
inline constexpr int kMaxDirEntries = 10000;
// A .gitignore past this is applied only up to the cap: generated ignore files
// can be huge; rules past the cap simply do not apply.
inline constexpr std::size_t kMaxGitignoreBytes = common::kMiB;
// Glob translation: nested braces multiply the output, so depth and size are
// capped far above every legitimate glob; past either the pattern is refused.
inline constexpr int kMaxBraceDepth = 16;
inline constexpr std::size_t kMaxTranslatedBytes = 64 * common::kKiB;

}  // namespace minimal::tools_search
