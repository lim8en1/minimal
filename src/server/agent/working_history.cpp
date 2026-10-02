#include "minimal/agent/working_history.hpp"

#include <algorithm>
#include <iterator>
#include <variant>

#include "minimal/agent/injection_queue.hpp"

namespace minimal::agent {

void WorkingHistory::Append(llm::Message message, std::optional<int> output_tokens,
                            const session::MessageAttribution& attribution) {
    store_.Append(message, output_tokens, attribution);
    // The persisted suffix gains its first member. Appends land at the END of
    // both the working set and the store, so this is the only place the boundary
    // can move forward.
    if (!first_persisted_seq_.has_value()) {
        first_persisted_seq_ = store_.last_appended_seq();
    }
    messages_.push_back(std::move(message));
}

bool WorkingHistory::EndsWithUnansweredUserText() const {
    if (messages_.empty()) return false;
    return IsUserTextMessage(messages_.back());
}

void WorkingHistory::DropThinking(std::size_t first, std::size_t last) {
    for (std::size_t i = first; i < last && i < messages_.size(); ++i) {
        auto& blocks = messages_[i].content;
        blocks.erase(std::remove_if(blocks.begin(), blocks.end(),
                                    [](const llm::ContentBlock& block) {
                                        return std::holds_alternative<llm::ThinkingBlock>(block);
                                    }),
                     blocks.end());
    }
}

void WorkingHistory::Clear() {
    messages_.clear();
    // The occupancy estimate goes with the history it described: a token count
    // that outlives its messages is the sort of thing a later change trips over,
    // and the compaction trigger reads it.
    tokens_ = llm::TokenAccumulator{};
    first_persisted_seq_.reset();
    head_is_summary_ = false;
    // Not the store. Compaction established that the in-memory working set and
    // the durable transcript are different things; /clear drops the first and
    // leaves scrollback whole.
}

WorkingHistory::TruncateResult WorkingHistory::TruncateAfter(std::int64_t target_seq) {
    // Nothing in memory to truncate. Reachable legitimately: a second rewind
    // after one to the transcript's start left the working set empty, and the
    // durable marks are still the Session's to make.
    if (messages_.empty()) return TruncateResult::kNothingToTruncate;
    if (!first_persisted_seq_.has_value() && !head_is_summary_) {
        return TruncateResult::kUnmapped;
    }
    // Index of the first message that must go. The contiguous-suffix invariant
    // makes this arithmetic exact: the persisted portion starts at
    // first_persisted_seq_, and the synthetic summary (when present) sits one
    // slot above the arithmetic.
    const std::int64_t base = first_persisted_seq_.value_or(0);
    const std::int64_t signed_idx = target_seq - base + (head_is_summary_ ? 1 : 0);
    // A rewind to the transcript's very start leaves the working set empty.
    const std::size_t idx = static_cast<std::size_t>(std::max<std::int64_t>(signed_idx, 0));
    messages_.erase(messages_.begin() + static_cast<long>(idx), messages_.end());
    if (messages_.empty()) {
        // An empty set has no persisted portion to be contiguous with, and the
        // next append establishes it. The summary flag goes with it: an empty
        // set has no synthetic head, and leaving it set would make the next
        // compaction subtract a phantom summary.
        first_persisted_seq_.reset();
        head_is_summary_ = false;
    }
    return TruncateResult::kTruncated;
}

void WorkingHistory::Reseed(std::vector<llm::Message> rows, std::int64_t first_seq) {
    messages_ = std::move(rows);
    first_persisted_seq_ =
        messages_.empty() ? std::nullopt : std::optional<std::int64_t>(first_seq);
    head_is_summary_ = false;
}

bool WorkingHistory::Restore(std::vector<llm::Message> rows, std::int64_t first_seq) {
    if (rows.empty()) return false;
    if (!first_persisted_seq_.has_value()) {
        // The working set was empty -- rewound to the transcript's start and
        // then redone. The restored rows ARE the persisted portion now.
        first_persisted_seq_ = first_seq;
        head_is_summary_ = false;
    }
    messages_.insert(messages_.end(), std::make_move_iterator(rows.begin()),
                     std::make_move_iterator(rows.end()));
    return true;
}

}  // namespace minimal::agent
