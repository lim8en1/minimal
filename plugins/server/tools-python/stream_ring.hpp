#pragma once

// The live-output ring for a running request: what `python_output` reads when
// the model polls a background task. Shaped like the shared sidecar
// CappedBuffer (head+tail, because a long log's first error and final status
// both matter) but with a different JOB and therefore a different interface:
// CappedBuffer persists a finished capture into a ToolResultBlock, while this
// one answers RANDOM-RANGE reads (`read:N:M` paging) over a still-growing
// stream with absolute byte offsets.
//
// Boundedness contract: head_cap + tail_cap per RUNNING request, period. Only
// the running request has a ring (the FIFO driver means at most one), the
// ring is discarded on completion (the capped per-request capture that rides
// the response line is the authoritative record), and nothing here grows with
// stream volume: everything past the head window and outside the tail window
// is DROPPED, reported as gap markers/lost counts rather than spilled
// anywhere. A future disk-spill backend slots in behind the same interface.
//
// NOT thread-safe: callers (the reader thread appending, tools reading)
// serialize on the interpreter mutex.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace minimal::tools_python {

class StreamRing {
public:
    StreamRing(std::size_t head_cap, std::size_t tail_cap)
        : head_cap_(head_cap), tail_cap_(tail_cap) {}

    void Append(std::string_view chunk) {
        if (chunk.empty()) return;
        total_ += chunk.size();
        // Head: the first head_cap bytes, frozen once full.
        if (head_.size() < head_cap_) {
            const auto take = std::min<std::size_t>(head_cap_ - head_.size(), chunk.size());
            head_.append(chunk.substr(0, take));
            chunk.remove_prefix(take);
        }
        if (chunk.empty()) return;
        // Tail: the last tail_cap bytes, overwrite-oldest.
        if (chunk.size() >= tail_cap_) {
            tail_.assign(chunk.substr(chunk.size() - tail_cap_));
        } else {
            tail_.append(chunk);
            if (tail_.size() > tail_cap_) tail_.erase(0, tail_.size() - tail_cap_);
        }
    }

    std::uint64_t total() const { return total_; }

    struct ReadResult {
        std::string text;         // retained slices, gap markers inline
        std::uint64_t total = 0;  // bytes ever appended (clamping reference)
        std::uint64_t next = 0;   // where the next Read should continue
        std::uint64_t lost = 0;   // bytes inside [offset, offset+len) not retained
    };

    // Reads [offset, offset + length) of the LOGICAL stream. Offsets are
    // absolute from the first byte ever appended; ranges that fell out of the
    // ring come back as an inline marker, counted in `lost`.
    ReadResult Read(std::uint64_t offset, std::size_t length) const {
        ReadResult out;
        out.total = total_;
        if (total_ == 0 || offset >= total_) {
            out.next = total_;
            return out;
        }
        const auto end = std::min(offset + static_cast<std::uint64_t>(length), total_);

        // The retained intervals of the logical stream. Head first; the tail
        // either continues it contiguously (while the stream is short enough
        // that the windows overlap) or starts after a real gap. Each interval
        // sits entirely inside ONE buffer, so Piece never straddles.
        Interval intervals[2];
        std::size_t n = 0;
        if (!head_.empty()) intervals[n++] = {0, head_.size()};
        if (!tail_.empty() && total_ > head_.size()) {
            const auto tail_begin = total_ - tail_.size();
            intervals[n++] = {tail_begin < head_.size() ? head_.size() : tail_begin, total_};
        }

        std::uint64_t cursor = offset;
        for (std::size_t i = 0; i < n && cursor < end; ++i) {
            const auto begin = std::max(intervals[i].begin, offset);
            const auto stop = std::min(intervals[i].end, end);
            if (stop <= begin) continue;  // interval outside the request
            if (begin > cursor) {
                const auto hole = begin - cursor;
                out.lost += hole;
                out.text += "\n[..., " + std::to_string(hole) + " bytes lost ...]\n";
            }
            out.text += Piece(begin, stop);
            cursor = stop;
        }
        if (cursor < end) {
            // The request ends inside dropped territory (past the tail
            // window's start, or past the head when the tail has not arrived).
            const auto hole = end - cursor;
            out.lost += hole;
            out.text += "\n[..., " + std::to_string(hole) + " bytes lost ...]\n";
            cursor = end;
        }
        out.next = end;
        return out;
    }

private:
    // [begin, end) inside ONE retained buffer: either entirely in the head or
    // entirely in the tail (Read's interval walk guarantees the split).
    std::string_view Piece(std::uint64_t begin, std::uint64_t end) const {
        if (begin < head_.size()) {
            return std::string_view(head_).substr(static_cast<std::size_t>(begin),
                                                  static_cast<std::size_t>(end - begin));
        }
        const auto tail_begin = total_ - tail_.size();
        return std::string_view(tail_).substr(static_cast<std::size_t>(begin - tail_begin),
                                              static_cast<std::size_t>(end - begin));
    }

    struct Interval {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };

    std::size_t head_cap_;
    std::size_t tail_cap_;
    std::string head_;       // first head_cap bytes, frozen when full
    std::string tail_;       // last tail_cap bytes, overwrite-oldest
    std::uint64_t total_ = 0;
};

}  // namespace minimal::tools_python
