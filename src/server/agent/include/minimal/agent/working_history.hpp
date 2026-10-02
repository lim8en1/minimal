#pragma once

// The loop's WORKING SET: the in-memory conversation the provider is sent, and
// the bookkeeping that ties it to the durable transcript. Distinct from the
// store on purpose -- compaction folds the working set into [summary + tail]
// while the store keeps every message, and /clear drops the working set and
// leaves scrollback whole.
//
// Invariant the seq arithmetic rests on: the working set's PERSISTED portion is
// a contiguous suffix of the store's active transcript. `first_persisted_seq`
// is where that suffix starts in the store's seq space, and `head_is_summary`
// says the first message is the synthetic summary (it exists only in memory).
// Appends land at the END of both, so Append is the only place the boundary
// moves forward.
//
// Worker-thread only, like everything on the loop but the injection queue.

#include <cstdint>
#include <optional>
#include <vector>

#include "minimal/llm/message.hpp"
#include "minimal/llm/token_accounting.hpp"
#include "minimal/session/history_store.hpp"

namespace minimal::agent {

class WorkingHistory {
public:
    // `history` / `first_persisted_seq` / `head_is_summary` are a resume seed
    // (empty for a fresh session).
    WorkingHistory(session::HistoryStore& store, std::vector<llm::Message> history,
                   std::optional<std::int64_t> first_persisted_seq, bool head_is_summary)
        : store_(store),
          messages_(std::move(history)),
          first_persisted_seq_(first_persisted_seq),
          head_is_summary_(head_is_summary) {}

    // Direct access for the loop's algorithms (compaction rewrites these in
    // place). The references stay valid for the object's life: nothing here ever
    // reseats them, it assigns through.
    std::vector<llm::Message>& messages() { return messages_; }
    const std::vector<llm::Message>& messages() const { return messages_; }
    std::optional<std::int64_t>& first_persisted_seq() { return first_persisted_seq_; }
    const std::optional<std::int64_t>& first_persisted_seq() const { return first_persisted_seq_; }
    bool& head_is_summary() { return head_is_summary_; }
    bool head_is_summary() const { return head_is_summary_; }
    llm::TokenAccumulator& tokens() { return tokens_; }
    const llm::TokenAccumulator& tokens() const { return tokens_; }

    // Persists `message` (with `attribution` for assistant rows) and then adds it
    // to the working set. Store first: a failed append must leave memory
    // untouched, or the two would disagree about what happened.
    void Append(llm::Message message, std::optional<int> output_tokens,
                const session::MessageAttribution& attribution);

    // The seq a compaction checkpoint would record as "covers everything up to
    // here": the persisted messages the summary head stands in for end just
    // before first_persisted_seq. nullopt when there is no summary head.
    std::optional<std::int64_t> CheckpointBoundary() const {
        if (head_is_summary_ && first_persisted_seq_.has_value()) return *first_persisted_seq_ - 1;
        return std::nullopt;
    }

    // True when the last message is a human prompt nobody has answered. TEXT, not
    // tool results: an injected subagent completion also ends the history in a
    // User message, but its blocks are ToolResultBlocks -- the model's own answers
    // arriving late, which wait for the next real turn.
    bool EndsWithUnansweredUserText() const;

    // Strips ThinkingBlocks from messages [first, last).
    void DropThinking(std::size_t first, std::size_t last);

    // /clear: the working set and its seq mapping and token estimate go; the
    // durable transcript is not touched.
    void Clear();

    enum class TruncateResult { kNothingToTruncate, kUnmapped, kTruncated };
    // Memory half of a rewind ABOVE the compaction boundary: drops every message
    // from `target_seq` on. kUnmapped means the set has no mapping to seq space
    // (defensive; callers validate against the store first).
    TruncateResult TruncateAfter(std::int64_t target_seq);

    // Memory half of a rewind AT or below the boundary: the set is rebuilt from
    // the store's active prefix, no summary head.
    void Reseed(std::vector<llm::Message> rows, std::int64_t first_seq);

    // Redo: appends rows the store already holds (they are NOT re-persisted).
    // False when `rows` is empty.
    bool Restore(std::vector<llm::Message> rows, std::int64_t first_seq);

private:
    session::HistoryStore& store_;
    std::vector<llm::Message> messages_;
    std::optional<std::int64_t> first_persisted_seq_;
    bool head_is_summary_ = false;
    llm::TokenAccumulator tokens_;
};

}  // namespace minimal::agent
