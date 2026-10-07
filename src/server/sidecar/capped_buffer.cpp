#include "minimal/sidecar/capped_buffer.hpp"

#include <algorithm>

namespace minimal::sidecar {

void CappedBuffer::Append(std::string_view chunk) {
    total_ += chunk.size();
    if (spill_) {
        // Nothing is dropped before the stream passes head_cap_ + tail_cap_
        // (Compact only erases past 2x the tail), so at the first overflow
        // head_ + tail_ is still the whole stream so far.
        if (!spilling_ && total_ > head_cap_ + tail_cap_) {
            spilling_ = true;
            spill_(head_);
            spill_(tail_);
        }
        if (spilling_) spill_(chunk);
    }
    if (head_.size() < head_cap_) {
        const auto take = std::min(head_cap_ - head_.size(), chunk.size());
        head_.append(chunk.data(), take);
        chunk.remove_prefix(take);
    }
    tail_.append(chunk.data(), chunk.size());

    // AMORTIZED, not per-chunk. Erasing down to exactly tail_cap_ on every
    // append means reads arriving in 8 KiB chunks memmove ~tail_cap_ bytes
    // each once the tail is full: a 200 MB build log -- an entirely ordinary
    // `make` -- is ~19 GB of memmove on the calling thread, spent purely on
    // throwing bytes away. Letting the tail grow to 2x before compacting
    // makes the work proportional to the data instead: each compaction
    // discards at least tail_cap_ bytes, so the total moved is O(total
    // output) rather than O(total output x tail_cap_ / chunk). The peak cost
    // is one extra tail_cap_ of memory, the same order the buffer already
    // holds.
    if (tail_.size() > 2 * tail_cap_) Compact();
}

std::string CappedBuffer::Render() {
    // Render is the only reader, and it needs the tail at its true size --
    // Append deliberately leaves it over-long between compactions.
    Compact();
    if (dropped_ == 0) return head_ + tail_;
    return head_ + "\n[" + std::to_string(dropped_) + " bytes omitted]\n" + tail_;
}

void CappedBuffer::Compact() {
    if (tail_.size() <= tail_cap_) return;
    dropped_ += tail_.size() - tail_cap_;
    tail_.erase(0, tail_.size() - tail_cap_);
}

}  // namespace minimal::sidecar
