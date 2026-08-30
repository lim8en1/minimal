#include "minimal/session/session_reader.hpp"

namespace minimal::session {

MessagePage SessionReader::GetMessages(const std::string& session_id,
                                       std::optional<std::int64_t> before_seq, int limit) const {
    return db_.GetMessagesPage(session_id, before_seq, limit);
}

std::vector<StoredMessage> SessionReader::LoadAll(const std::string& session_id) const {
    return db_.LoadMessages(session_id);
}

}  // namespace minimal::session
