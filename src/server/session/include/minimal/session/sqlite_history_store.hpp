#pragma once

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

    void Append(const llm::Message& message, std::optional<int> output_tokens = std::nullopt) override;

private:
    SessionDatabase& db_;
    std::string session_id_;
};

}  // namespace minimal::session
