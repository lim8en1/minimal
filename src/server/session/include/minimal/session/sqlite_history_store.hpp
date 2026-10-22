#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "minimal/session/history_store.hpp"
#include "minimal/session/session_database.hpp"

namespace minimal::session {

// The durable HistoryStore: one instance per session, all of them sharing the
// process-wide SessionDatabase (and its single connection/mutex). The AgentLoop
// sees only the one-method interface; seq assignment and the updated_at touch
// are SessionDatabase concerns, applied in one transaction. Default naming is
// agent::Session's domain rule (agent/session_naming.hpp) now -- this class
// only stores what it is told, via SetDefaultNameIfUnset.
class SqliteHistoryStore : public HistoryStore {
public:
    SqliteHistoryStore(SessionDatabase& db, std::string session_id);

    void Append(const llm::Message& message, std::optional<int> output_tokens,
                const MessageAttribution& attribution) override;

    void AppendRound(const llm::Message& assistant, std::optional<int> output_tokens,
                     const MessageAttribution& attribution, const llm::Message& results) override;

    // Cached from the database's own answer rather than counted locally, so it
    // stays correct for a session that did not start empty. Atomic because the
    // session worker writes it while the observer path reads it.
    std::int64_t last_appended_seq() const override {
        return last_seq_.load(std::memory_order_acquire);
    }

    CompactionStamp SaveCompactionCheckpoint(std::int64_t boundary_seq, const llm::Message& summary,
                                  const CompactionMeta& meta) override;
    void ClearCompactionCheckpoint() override;

    // --- Session traversal (rewind) ------------------------------------------
    // Thin delegates; the transactions and the active-set semantics live in
    // SessionDatabase. Both re-read last_seq_ from the ACTIVE transcript: a
    // rewind moves the tail, and until the next append reclaims it every
    // consumer of last_appended_seq() (the next turn's first_seq, a
    // compaction's at_seq) must see the tail the user sees.
    bool SupportsRewind() const override { return true; }
    std::int64_t SetDiscardedAfter(std::int64_t target_seq) override;
    std::int64_t UnmarkDiscarded() override;

    bool SetDefaultNameIfUnset(const std::string& name) override;
    bool Rename(const std::string& name) override;
    void PersistModelOverride(const std::optional<std::string>& provider_model) override;
    void PersistSubagentModel(const std::optional<std::string>& provider_model) override;

private:
    SessionDatabase& db_;
    std::string session_id_;
    std::atomic<std::int64_t> last_seq_{-1};
};

}  // namespace minimal::session
