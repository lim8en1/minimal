#pragma once

#include <cstdint>
#include <string>

#include "minimal/session/session_database.hpp"
#include "minimal/session/stored_message.hpp"

namespace minimal::session {

// Read-only history access, the same path GetMessages will use. Plugins receive
// this through the ABI utilities half instead of touching SQLite directly; it is
// deliberately limited to reads -- writes go through a session's HistoryStore.
class SessionReader {
public:
    explicit SessionReader(SessionDatabase& db) : db_(db) {}

    // Keyset page, descending by seq. before_seq is an exclusive upper bound;
    // <= 0 starts at the most recent message. limit must be > 0 (the RPC layer
    // applies settings.memory.scrollback_page_size as its default).
    MessagePage GetMessages(const std::string& session_id, std::int64_t before_seq, int limit) const;

private:
    SessionDatabase& db_;
};

}  // namespace minimal::session
