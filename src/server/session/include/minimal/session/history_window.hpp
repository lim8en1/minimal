#pragma once

#include <algorithm>
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
// memory proportional to the page size and never to history size -- and the
// page size is capped, so no single call can materialize the transcript no
// matter what a tool asks for. (A tool paging in a loop through has_more can
// still read everything -- deliberately, a bounded window at a time.)
class HistoryWindow {
public:
    // The ceiling on one page through the window. Generous against any sane
    // paging tool (the scrollback RPC defaults to 100), small against a
    // transcript a tool has no business pulling whole in one call.
    static constexpr int kMaxPageLimit = 1000;

    HistoryWindow(const SessionReader& reader, std::string session_id)
        : reader_(&reader), session_id_(std::move(session_id)) {}

    // Keyset page of THIS window's transcript, descending by seq. before_seq is
    // an exclusive upper bound; nullopt starts at the most recent message.
    // limit is clamped into [1, kMaxPageLimit] rather than rejected -- a
    // tool-facing surface degrades a bad page size, it does not throw out of
    // the tool call. Delegates to SessionReader::GetMessages -- the same
    // cursor semantics, unchanged.
    MessagePage GetMessages(std::optional<std::int64_t> before_seq, int limit) const {
        limit = std::clamp(limit, 1, kMaxPageLimit);
        return reader_->GetMessages(session_id_, std::move(before_seq), limit);
    }

    const std::string& session_id() const { return session_id_; }

private:
    const SessionReader* reader_;
    std::string session_id_;
};

}  // namespace minimal::session
