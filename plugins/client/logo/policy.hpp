#pragma once

// logo policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::logo {

// Rendered logos cached (per session key): more than a process has open or
// recently open at once; each is a handful of short lines.
inline constexpr std::size_t kCacheCap = 200;

// Blank columns between letters.
inline constexpr int kLetterGap = 1;

}  // namespace minimal::logo
