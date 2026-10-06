#pragma once

// session policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

namespace minimal::session {

// Ceiling on one page of messages, for every pager (the scrollback RPCs and
// HistoryWindow alike). Load-bearing: the store binds `limit + 1` to probe for
// more, and an unbounded page pulls a whole transcript in one response -- the
// O(n) blowup keyset paging exists to prevent. Generous against any sane pager.
inline constexpr int kMaxHistoryPageSize = 1000;

// Rows handled per lock hold when deleting or loading a whole transcript.
// Releasing the lock between chunks lets other sessions' appends interleave
// with a long delete or resume; a few hundred rows keep each hold short.
inline constexpr int kRowsPerLockedChunk = 500;

// Appends between passive WAL checkpoints (and between retries of a failed
// one): keeps the WAL from growing without bound while a busy session
// writes, without checkpointing on every append.
inline constexpr int kCheckpointAppends = 200;

}  // namespace minimal::session
