#include "minimal/llm/sse_parser.hpp"

#include <stdexcept>

namespace minimal::llm {
namespace {

std::string_view strip_cr(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

// A stream with no newline -- or a `data:` firehose that never sends the blank
// line that completes a frame -- grew line_buffer_ / pending_data_ without
// bound. 1 MiB is far above any legitimate SSE line (deltas are small JSON
// objects); past it the stream is broken, and the failure is thrown so the
// provider's existing catch-and-classify machinery turns it into an LlmError
// rather than letting a wedged proxy consume memory until the idle timeout.
constexpr std::size_t kMaxPendingBytes = 1ull << 20;

}  // namespace

std::vector<SseFrame> SseParser::Feed(std::string_view chunk) {
    if (line_buffer_.size() + chunk.size() > kMaxPendingBytes) {
        throw std::runtime_error("SSE line exceeded 1 MiB without a newline");
    }
    line_buffer_.append(chunk);

    std::vector<SseFrame> frames;
    std::size_t start = 0;
    while (true) {
        const std::size_t nl = line_buffer_.find('\n', start);
        if (nl == std::string::npos) break;
        const std::string_view line = strip_cr(std::string_view(line_buffer_).substr(start, nl - start));
        start = nl + 1;

        if (line.empty()) {
            if (pending_has_data_) {
                SseFrame frame;
                frame.event = pending_event_;
                frame.id = pending_id_;
                frame.data = pending_data_;
                if (!frame.data.empty() && frame.data.back() == '\n') frame.data.pop_back();
                frames.push_back(std::move(frame));
            }
            pending_event_ = "message";
            pending_data_.clear();
            pending_id_.clear();
            pending_has_data_ = false;
            continue;
        }
        if (line.front() == ':') continue;  // comment line, ignored

        const std::size_t colon = line.find(':');
        std::string_view field, value;
        if (colon == std::string_view::npos) {
            field = line;
        } else {
            field = line.substr(0, colon);
            value = line.substr(colon + 1);
            if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        }

        if (field == "event") {
            pending_event_.assign(value);
        } else if (field == "data") {
            if (pending_data_.size() + value.size() > kMaxPendingBytes) {
                throw std::runtime_error(
                    "SSE frame exceeded 1 MiB of data without a completing blank line");
            }
            pending_data_.append(value);
            pending_data_.push_back('\n');
            pending_has_data_ = true;
        } else if (field == "id") {
            pending_id_.assign(value);
        }
        // Unknown fields (e.g. "retry") are ignored per the SSE spec.
    }
    line_buffer_.erase(0, start);
    return frames;
}

}  // namespace minimal::llm
