#include "minimal/llm/sse_parser.hpp"

namespace minimal::llm {
namespace {

std::string_view strip_cr(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    return line;
}

}  // namespace

std::vector<SseFrame> SseParser::Feed(std::string_view chunk) {
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
