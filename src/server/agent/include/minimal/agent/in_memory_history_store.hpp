#pragma once

// The transcript store used until the SQLite one lands.
//
// Deliberately NOT the test fake promoted verbatim. Two things are real here
// that a fake does not need:
//
//   - It is thread-safe. The session worker appends mid-turn while GetMessages
//     reads concurrently, which is the whole reason HistoryStore writes per
//     message rather than once at the end of a turn.
//   - It has a seq-keyed read side. GetMessages is keyset-paginated by seq, so
//     building that shape now means the SQLite implementation swaps in behind an
//     interface that already has the right questions on it, rather than forcing
//     a second interface change later.
//
// What it does not have is durability. A session's transcript lives as long as
// the server process and no longer -- see ARCHITECTURE.md "Session storage".

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "minimal/agent/history_store.hpp"

namespace minimal::agent {

struct StoredMessage {
    std::int64_t seq = 0;
    llm::Message message;
};

class InMemoryHistoryStore : public HistoryStore {
public:
    void Append(const llm::Message& message) override;

    // Descending by seq, matching GetMessagesResponse's contract: scrollback
    // pages backwards from the newest message. `before_seq <= 0` means "start
    // from the most recent"; `limit <= 0` means no limit.
    std::vector<StoredMessage> Read(std::int64_t before_seq, int limit) const;

    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<StoredMessage> messages_;
    std::int64_t next_seq_ = 0;
};

}  // namespace minimal::agent
