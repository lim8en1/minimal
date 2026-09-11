#pragma once

// The head+tail capped output buffer, shared by every tool that captures
// child output. Before this module tools-bash and tools-python each kept a
// copy of the same algorithm.
//
// Head (1/4 of the cap) + tail (3/4): in a long build log both the first error
// and the final status matter. This bounds only what is PERSISTED into the
// ToolResultBlock; live streaming through emit_output is uncapped because that
// channel is lossy by design -- the session's outbound queue is the backpressure
// boundary, and the authoritative record is the capped result here.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace minimal::sidecar {

class CappedBuffer {
public:
    explicit CappedBuffer(std::int64_t cap)
        : head_cap_(static_cast<std::size_t>(cap / 4)),
          tail_cap_(static_cast<std::size_t>(cap - cap / 4)) {}

    void Append(std::string_view chunk);
    std::string Render();

private:
    void Compact();

    std::size_t head_cap_, tail_cap_;
    std::string head_, tail_;
    std::size_t dropped_ = 0;
};

}  // namespace minimal::sidecar
