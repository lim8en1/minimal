#pragma once

// markdown-md4c policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::client::markdown_md4c {

// Markdown of a streaming message is reparsed whole (md4c has no incremental
// API), at most this often; past the threshold the interval grows with the
// buffer up to the ceiling, so a long reply's tail still updates without the
// reparse eating every frame.
inline constexpr std::chrono::milliseconds kMarkdownReparseDebounce{120};
inline constexpr std::chrono::milliseconds kMaxMarkdownReparseDebounce{500};
inline constexpr std::size_t kMarkdownReparseScaleThresholdBytes = 8 * common::kKiB;
// Past the threshold the interval grows by a millisecond per this many bytes.
inline constexpr std::size_t kMarkdownReparseBytesPerExtraMs = 64;

}  // namespace minimal::client::markdown_md4c
