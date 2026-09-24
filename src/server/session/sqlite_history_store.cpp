#include "minimal/session/sqlite_history_store.hpp"

namespace minimal::session {

SqliteHistoryStore::SqliteHistoryStore(SessionDatabase& db, std::string session_id)
    : db_(db),
      session_id_(std::move(session_id)),
      // Read from the database rather than assumed empty: a RESUMED session
      // starts with its stored transcript already persisted, and its next
      // append must continue that sequence. Getting this wrong would restart
      // last_appended_seq at -1, which SQL's MAX(seq)+1 would silently
      // contradict on the first append -- and TurnSummary's seq bounds would
      // report values that belong to the previous incarnation of the session.
      last_seq_(db.LastMessageSeq(session_id_)) {}

void SqliteHistoryStore::Append(const llm::Message& message, std::optional<int> output_tokens,
                                const MessageAttribution& attribution) {
    const std::int64_t seq =
        db_.AppendMessage(session_id_, message, output_tokens.value_or(0), attribution);
    last_seq_.store(seq, std::memory_order_release);
}

void SqliteHistoryStore::SaveCompactionCheckpoint(std::int64_t boundary_seq,
                                                  const llm::Message& summary,
                                                  const CompactionMeta& meta) {
    db_.SaveCompactionCheckpoint(session_id_, boundary_seq, summary, meta);
}

void SqliteHistoryStore::ClearCompactionCheckpoint() {
    db_.ClearCompactionCheckpoint(session_id_);
}

std::int64_t SqliteHistoryStore::SetDiscardedAfter(std::int64_t target_seq) {
    return db_.SetDiscardedAfter(session_id_, target_seq);
}

std::int64_t SqliteHistoryStore::UnmarkDiscarded() {
    return db_.UnmarkDiscarded(session_id_);
}

bool SqliteHistoryStore::SetDefaultNameIfUnset(const std::string& name) {
    return db_.SetDefaultName(session_id_, name);
}

bool SqliteHistoryStore::Rename(const std::string& name) {
    return db_.UpdateSessionName(session_id_, name, "explicit");
}

void SqliteHistoryStore::PersistModelOverride(const std::optional<std::string>& provider_model) {
    db_.UpdateSessionModel(session_id_, provider_model);
}

void SqliteHistoryStore::PersistSubagentModel(const std::optional<std::string>& provider_model) {
    db_.UpdateSessionSubagentModel(session_id_, provider_model);
}

}  // namespace minimal::session
