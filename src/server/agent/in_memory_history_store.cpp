#include "minimal/agent/in_memory_history_store.hpp"

namespace minimal::agent {

void InMemoryHistoryStore::Append(const llm::Message& message, std::optional<int> output_tokens) {
    (void)output_tokens;  // durability concern; this store has none
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(StoredMessage{next_seq_++, message});
}

std::vector<StoredMessage> InMemoryHistoryStore::Read(std::int64_t before_seq, int limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    // seq is dense and assigned in order here, so it doubles as the index. The
    // SQLite implementation will not have that luxury -- hence the API taking a
    // seq rather than an offset, so callers never learn to depend on it.
    std::int64_t upper = static_cast<std::int64_t>(messages_.size());
    if (before_seq > 0 && before_seq < upper) upper = before_seq;

    std::vector<StoredMessage> out;
    for (std::int64_t i = upper - 1; i >= 0; --i) {
        if (limit > 0 && static_cast<int>(out.size()) >= limit) break;
        out.push_back(messages_[static_cast<std::size_t>(i)]);
    }
    return out;
}

std::size_t InMemoryHistoryStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return messages_.size();
}

}  // namespace minimal::agent
