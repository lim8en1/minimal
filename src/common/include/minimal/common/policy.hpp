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

// A SerialExecutor's queue for notification-style tasks (session observers,
// async hook actions). Each task holds a copy of its payload, so EventQueue's
// stream-sized default would reserve tens of MB for a backlog that means "the
// consumer stopped" long before it fills; a few hundred pending is already
// pathological.
inline constexpr std::size_t kDefaultNotificationQueueCapacity = 1024;

// Small config documents read whole and parsed at once -- agent presets,
// commands, skills, themes, the trusted-directories file. Kilobytes in
// practice; several of these come from untrusted project directories, and
// their parsers have no size or depth limit of their own, so anything past
// this is refused before parsing.
inline constexpr std::size_t kMaxConfigFileBytes = kMiB;

// gRPC message limit, the same both ways and on both ends: well above the
// largest legitimate message (a capped tool result inside an event, a rewind
// payload), where gRPC's own defaults were asymmetric (4 MB in, unlimited out).
inline constexpr std::size_t kMaxWireMessageBytes = 64 * kMiB;

// TCP connect budget for any HTTP request that does not set its own: a
// reachable host connects in well under a second; an unreachable one is
// reported in seconds.
inline constexpr std::chrono::milliseconds kDefaultConnectTimeout = std::chrono::seconds(5);

// Slice for a wait that polls a cancellation (retry backoff, a blocking
// subagent, an advisor's answer): the bound on how late a cancel is noticed.
// Human scale; shorter only multiplies wake-ups over a long wait.
inline constexpr std::chrono::milliseconds kCancelPollSlice{100};

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
