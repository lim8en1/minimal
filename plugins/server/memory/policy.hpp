#pragma once

// memory policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "minimal/common/units.hpp"

namespace minimal::memory {

// --- plugins.json memory.config defaults ------------------------------------------------

// http embeddings: one request's whole transfer.
inline constexpr std::chrono::milliseconds kDefaultEmbedTimeout = std::chrono::seconds(30);
// sidecar embeddings: the first start may download model weights; later
// requests are one batch's round trip.
inline constexpr std::chrono::milliseconds kDefaultSidecarStartupTimeout = std::chrono::minutes(1);
inline constexpr std::chrono::milliseconds kDefaultSidecarRequestTimeout = std::chrono::seconds(30);
// Tagging: one provider call over a batch of entries -- room for a JSON reply
// naming tags for each, bounded in time like any background call.
inline constexpr int kDefaultTaggerMaxTokens = 512;
inline constexpr std::chrono::milliseconds kDefaultTaggerTimeout = std::chrono::minutes(1);
// The tag vocabulary: enough tags per scope to file a project's lessons,
// few per entry so a tag still means something; a proposed tag this similar
// to an existing one is merged into it.
inline constexpr int kDefaultMaxTags = 100;
inline constexpr int kDefaultMaxTagsPerEntry = 5;
inline constexpr double kDefaultTagMergeSimilarity = 0.85;
// One entry's body (and one ingest chunk): a lesson, not a document.
inline constexpr int kDefaultMaxTextBytes = 8 * static_cast<int>(common::kKiB);
// Inputs per embeddings request: a batch most backends take in one call.
inline constexpr int kDefaultEmbedRequestBatch = 64;
// Search results by default, and the ceiling on a model-supplied limit: a
// handful of lessons answers a question; past the ceiling it is a dump.
inline constexpr int kDefaultSearchLimit = 12;
inline constexpr int kSearchLimitCeiling = 64;
// The "Memory" section pre-filled into sessions (~a thousand tokens): pinned
// bodies, the tag vocabulary, recent one-liners, in that order.
inline constexpr int kDefaultIndexBudgetBytes = 4000;

// Floors below which a setting is degenerate, not a tuning choice.
inline constexpr int kMinTextBytes = 256;           // an entry body with room for a sentence
inline constexpr int kMinIndexBudgetBytes = 512;    // the section header and a line or two
inline constexpr double kMinTagMergeSimilarity = 0.0;  // similarity, not distance

// --- entries -------------------------------------------------------------------------------

// A stored entry's title and description (single lines: they render in lists
// and the context index); ingest derives shorter ones from a file's headings
// and first sentence, leaving room for an edit.
inline constexpr std::size_t kMaxTitleChars = 120;
inline constexpr std::size_t kMaxDescriptionChars = 300;
inline constexpr std::size_t kIngestTitleChars = 80;
inline constexpr std::size_t kIngestDescriptionChars = 200;
// A just-stored entry this similar to an existing one is reported as a likely
// duplicate.
inline constexpr double kDuplicateSimilarity = 0.92;
// memory_store's file ingestion: notes, not a corpus -- refused before reading.
inline constexpr std::uintmax_t kMaxIngestFileBytes = 2 * common::kMiB;
// What is embedded per entry: title and description lead, so the cap trims
// only the body's tail; small local embedding models take ~512 tokens.
inline constexpr std::size_t kEmbedTextMaxChars = 2000;
// A tag slug's length.
inline constexpr std::size_t kMaxTagBytes = 32;
// memory_get: ids per call.
inline constexpr std::size_t kGetMaxIds = 20;
// Scores shown to the model are rounded to this many per unit (three
// decimals): enough to rank by eye.
inline constexpr double kScoreScale = 1000.0;

// --- search ---------------------------------------------------------------------------------

// Query terms kept for the full-text and the substring matchers: a question,
// not a paragraph.
inline constexpr std::size_t kMaxFtsTerms = 32;
inline constexpr std::size_t kMaxSubstringTerms = 16;
// Reciprocal-rank fusion's k (the standard 60): dampens the gap between rank
// 1 and 2 so one list cannot dominate on a single strong hit.
inline constexpr double kRrfK = 60.0;
// A vector-only hit must reach this fraction of the best cosine to count, so a
// small store does not pad every answer with noise.
inline constexpr double kVectorRelativeFloor = 0.5;

// --- context index ----------------------------------------------------------------------

// Recent one-liners fetched per render: more than any sane budget fits, so
// the budget binds; and the room kept for the section's own header.
inline constexpr int kRecentCandidates = 200;
inline constexpr std::size_t kIndexHeaderRoomBytes = 80;

// --- background workers (embedding reindex, tagger) ---------------------------------------

// Rows a worker looks at per pass; the tagger's entry text per provider call
// and the neighbors it shows the model for context.
inline constexpr int kReindexBatchRows = 32;
inline constexpr int kTaggerPendingScan = 32;
inline constexpr std::size_t kTaggerBatchBytes = 24000;
inline constexpr int kTaggerNeighbors = 5;
// Idle workers recheck this often (a write wakes them at once).
inline constexpr std::chrono::milliseconds kWorkerIdlePoll = std::chrono::minutes(1);
// A failing backend's circuit breaker: cooldown doubling from the base to the
// ceiling; a worker wakes this long after the cooldown ends.
inline constexpr std::chrono::milliseconds kBreakerBase = std::chrono::seconds(30);
inline constexpr std::chrono::milliseconds kBreakerMax = std::chrono::minutes(5);
inline constexpr std::chrono::milliseconds kBreakerWakeSlack{50};

// --- embedding sidecar -----------------------------------------------------------------------

// Sidecar diagnostics kept for error reports, and the protocol line ceiling (a
// big reindex batch's embeddings as JSON; past this is corruption).
inline constexpr std::size_t kSidecarMaxDiagBytes = 16 * common::kKiB;
inline constexpr std::size_t kSidecarMaxLineBytes = 256 * common::kMiB;

}  // namespace minimal::memory
