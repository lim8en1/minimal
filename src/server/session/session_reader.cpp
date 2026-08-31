#include "minimal/session/session_reader.hpp"

namespace minimal::session {

MessagePage SessionReader::GetMessages(const std::string& session_id,
                                       std::optional<std::int64_t> before_seq, int limit) const {
    return db_.GetMessagesPage(session_id, before_seq, limit);
}

std::vector<StoredMessage> SessionReader::LoadAll(const std::string& session_id) const {
    return db_.LoadMessages(session_id);
}

std::vector<StoredMessage> SessionReader::LoadAfter(const std::string& session_id,
                                                    std::int64_t after_seq) const {
    return db_.LoadMessagesFrom(session_id, after_seq);
}

std::optional<StoredMessage> SessionReader::LoadMessage(const std::string& session_id,
                                                        std::int64_t seq) const {
    return db_.LoadMessage(session_id, seq);
}

std::int64_t SessionReader::ActiveLastSeq(const std::string& session_id) const {
    return db_.LastMessageSeq(session_id);
}

}  // namespace minimal::session
