#include "minimal/session/session_reader.hpp"

namespace minimal::session {

MessagePage SessionReader::GetMessages(const std::string& session_id, std::int64_t before_seq,
                                       int limit) const {
    return db_.GetMessagesPage(session_id, before_seq, limit);
}

}  // namespace minimal::session
