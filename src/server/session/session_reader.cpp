#include "minimal/session/session_reader.hpp"

#include <algorithm>

namespace minimal::session {

namespace {

// The compaction records that ran at one of this page's seqs. Every record's
// at_seq is the seq of an active message (the last one when it ran), so each
// record belongs to exactly the one page holding that message.
void AttachCompactions(const SessionDatabase& db, const std::string& session_id,
                       MessagePage& page) {
    if (page.messages.empty()) return;
    std::int64_t low = page.messages.front().seq;
    std::int64_t high = low;
    for (const auto& stored : page.messages) {
        low = std::min(low, stored.seq);
        high = std::max(high, stored.seq);
    }
    page.compactions = db.LoadCompactionRecords(session_id, low, high);
}

}  // namespace

MessagePage SessionReader::GetMessages(const std::string& session_id,
                                       std::optional<std::int64_t> before_seq, int limit) const {
    MessagePage page = db_.GetMessagesPage(session_id, before_seq, limit);
    AttachCompactions(db_, session_id, page);
    return page;
}

MessagePage SessionReader::GetMessagesAfter(const std::string& session_id,
                                            std::int64_t after_seq, int limit) const {
    MessagePage page = db_.GetMessagesPageAfter(session_id, after_seq, limit);
    AttachCompactions(db_, session_id, page);
    return page;
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
