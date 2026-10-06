#pragma once

// sidecar policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

#include "minimal/common/units.hpp"

namespace minimal::sidecar {

// Drain-poll slice: the bound on how late a cancel or a deadline is noticed
// while a child is quiet. Well under human reaction time; long enough that
// an idle child costs no measurable CPU.
inline constexpr std::chrono::milliseconds kDrainSlice{50};

// WNOHANG poll interval while waiting for a child to exit (after EOF, after a
// kill, during a terminate grace). A child usually exits within a few of
// these; shorter only burns wake-ups.
inline constexpr std::chrono::milliseconds kReapPollInterval{10};

// Ceiling on the reap after SIGKILL. SIGKILL ends any process not stuck in
// uninterruptible disk sleep (D state) almost at once; this only bounds that
// case, so the caller reports an undetermined status instead of hanging.
// Generous because giving up leaves a zombie.
inline constexpr std::chrono::milliseconds kPostKillReapCeiling{5000};

// One read() from a child's pipe. A pipe buffers tens of KiB; a fraction of
// that per read keeps the drain loop turning over the other pipes and
// cancellation between reads.
inline constexpr std::size_t kPipeReadChunkBytes = 8 * common::kKiB;

// How far RLIMIT_CPU sits past a wall-clock deadline it backs up. The limit
// only exists for what the deadline's group kill cannot reach (a descendant
// that left the group keeps the inherited limit); it must never fire first on
// a healthy child, or the run dies of SIGXCPU instead of reporting "timed
// out". CPU time is summed over threads and a loaded machine stretches wall
// time unevenly, so the margin is generous: python runs under load were the
// case that needed it.
inline constexpr std::chrono::seconds kCpuBackstopGrace{30};

// Finished background-task records kept per session for /tasks; running ones
// are never dropped. Enough to look back over a working session; the oldest
// go first.
inline constexpr std::size_t kMaxFinishedTaskRecordsPerSession = 32;

}  // namespace minimal::sidecar
