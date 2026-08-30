#include "minimal/session/sqlite_history_store.hpp"

namespace minimal::session {

SqliteHistoryStore::SqliteHistoryStore(SessionDatabase& db, std::string session_id)
    : db_(db), session_id_(std::move(session_id)) {}

void SqliteHistoryStore::Append(const llm::Message& message, std::optional<int> output_tokens) {
    db_.AppendMessage(session_id_, message, output_tokens.value_or(0));
}

}  // namespace minimal::session
