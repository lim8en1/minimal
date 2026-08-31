#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "minimal/session/session_reader.hpp"

namespace minimal::session {

// The tool-facing slice of SessionReader: windowed reads only, bound to ONE
// session id at construction. Tools receive this through ToolContext::history
// (docs/EXTENSIBILITY.md §2.1, track M); it exists so that the windowed-only
// rule is a property of the type rather than a comment -- there is no LoadAll
// here to reach. Full materialization stays the resume path's job, on
// SessionReader, which no tool holds.
//
// Non-owning and cheap: the reader is owned by the Session, which also owns
// this window (and destroys both in an order that keeps the pointer valid --
// the window dies with the loop that borrows it, never before). GetMessages
// pages exactly like the GetMessages RPC: a keyset page, descending by seq,
// memory proportional to `limit` and never to history size, so reading through
// this window cannot violate the 512k-1M goal no matter what a tool asks for.
class HistoryWindow {
public:
    HistoryWindow(const SessionReader& reader, std::string session_id)
        : reader_(&reader), session_id_(std::move(session_id)) {}

    // Keyset page of THIS window's transcript, descending by seq. before_seq is
    // an exclusive upper bound; nullopt starts at the most recent message.
    // limit must be > 0. Delegates to SessionReader::GetMessages -- the same
    // cursor semantics, unchanged.
    MessagePage GetMessages(std::optional<std::int64_t> before_seq, int limit) const {
        return reader_->GetMessages(session_id_, std::move(before_seq), limit);
    }

    const std::string& session_id() const { return session_id_; }

private:
    const SessionReader* reader_;
    std::string session_id_;
};

}  // namespace minimal::session
