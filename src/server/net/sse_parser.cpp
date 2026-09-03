#include "minimal/net/sse_parser.hpp"

#include <stdexcept>

namespace minimal::net {
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
    // The spec strips one leading UTF-8 BOM from the stream. Without this, the
    // first data line's field name became "\xEF\xBB\xBFdata" -- unknown field,
    // silently ignored -- and the first OpenAI chunk (id/model/role) was
    // dropped from every BOM-prefixed stream.
    if (!bom_checked_) {
        bom_checked_ = true;
        if (chunk.size() >= 3 && static_cast<unsigned char>(chunk[0]) == 0xEF &&
            static_cast<unsigned char>(chunk[1]) == 0xBB &&
            static_cast<unsigned char>(chunk[2]) == 0xBF) {
            chunk.remove_prefix(3);
        }
    }
    if (line_buffer_.size() + chunk.size() > kMaxPendingBytes) {
        throw std::runtime_error("SSE line exceeded 1 MiB without a newline");
    }
    line_buffer_.append(chunk);

    std::vector<SseFrame> frames;
    std::size_t start = 0;
    while (true) {
        // The spec allows LF, CRLF, and a lone CR as line terminators. Only
        // '\n' was recognized: a CR-terminated stream never framed at all and
        // died at the 1 MiB cap. A '\r' that is the LAST byte held is not a
        // terminator yet -- it may be the first half of a CRLF split across
        // chunks, and terminating there would let the next chunk's '\n' flush
        // a partial frame as if it were complete.
        const std::size_t nl = line_buffer_.find('\n', start);
        const std::size_t cr = line_buffer_.find('\r', start);
        std::size_t eol;
        std::size_t advance;
        if (nl != std::string::npos && (cr == std::string::npos || nl < cr)) {
            eol = nl;
            advance = 1;
        } else if (cr != std::string::npos) {
            if (cr + 1 == line_buffer_.size()) break;  // wait for the byte after '\r'
            eol = cr;
            advance = line_buffer_[cr + 1] == '\n' ? 2 : 1;
        } else {
            break;
        }
        const std::string_view line =
            strip_cr(std::string_view(line_buffer_).substr(start, eol - start));
        start = eol + advance;

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
            // An empty event value is the spec's "message", not a name.
            pending_event_.assign(value.empty() ? "message" : value);
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

}  // namespace minimal::net
