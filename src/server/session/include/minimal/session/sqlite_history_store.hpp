#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "minimal/agent/history_store.hpp"
#include "minimal/session/session_database.hpp"

namespace minimal::session {

// The durable HistoryStore: one instance per session, all of them sharing the
// process-wide SessionDatabase (and its single connection/mutex). The AgentLoop
// sees only the one-method interface; seq assignment, default naming, and the
// updated_at touch are SessionDatabase concerns, applied in one transaction.
class SqliteHistoryStore : public agent::HistoryStore {
public:
    SqliteHistoryStore(SessionDatabase& db, std::string session_id);

    void Append(const llm::Message& message, std::optional<int> output_tokens) override;

    // Cached from the database's own answer rather than counted locally, so it
    // stays correct for a session that did not start empty. Atomic because the
    // session worker writes it while the observer path reads it.
    std::int64_t last_appended_seq() const override {
        return last_seq_.load(std::memory_order_acquire);
    }

    void SaveCompactionCheckpoint(std::int64_t boundary_seq, const llm::Message& summary) override;
    void ClearCompactionCheckpoint() override;

    // --- Session traversal (rewind) ------------------------------------------
    // Thin delegates; the transactions and the active-set semantics live in
    // SessionDatabase. last_seq_ is deliberately left alone by both: it tracks
    // the last APPENDED seq, and neither a mark nor an unmark appends.
    bool SupportsRewind() const override { return true; }
    std::int64_t SetDiscardedAfter(std::int64_t target_seq) override;
    std::int64_t UnmarkDiscarded() override;

private:
    SessionDatabase& db_;
    std::string session_id_;
    std::atomic<std::int64_t> last_seq_{-1};
};

}  // namespace minimal::session
