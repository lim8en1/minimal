#pragma once

// common/ policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::common {

// ReadFile's default cap. Callers read prompts, presets and small JSON
// documents, not data files: far past any of those, small enough that a
// mistaken path to a disk image fails instead of exhausting memory. Callers
// that read working-tree content pass their own, tighter cap.
inline constexpr std::size_t kDefaultReadFileMaxBytes = 64 * kMiB;

// Stack buffer for a bounded file read. Config-sized files fit in a read or
// two; larger buys nothing and costs stack.
inline constexpr std::size_t kReadChunkBytes = 16 * kKiB;

// EventQueue's default bound: catches a runaway producer, not a tuning knob.
// Far above any backlog a healthy consumer leaves, finite so a stalled one
// blocks its producer instead of growing without limit.
inline constexpr std::size_t kDefaultEventQueueMaxSize = 65536;

// Log rotation defaults (settings.json logging.max_size_mb / max_files): disk
// use is bounded by their product, and a few rotated files keep the
// history before a problem without anyone pruning by hand.
inline constexpr std::int64_t kDefaultLogMaxSizeMb = 10;
inline constexpr std::int64_t kDefaultLogMaxFiles = 5;

// PeriodicTask's period while its `interval` callback has never answered
// (it threw on the first call): frequent enough that the task still runs,
// rare enough that a broken callback does not spin.
inline constexpr std::chrono::milliseconds kPeriodicTaskFallbackInterval{1000};

// How much of an offending value a config error quotes: enough to recognize
// it, short enough to keep the error on one line.
inline constexpr std::size_t kConfigErrorValuePreviewChars = 80;

}  // namespace minimal::common
