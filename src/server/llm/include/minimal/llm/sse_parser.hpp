#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace minimal::llm {

struct SseFrame {
    std::string event = "message";
    std::string data;
    std::string id;
};

// Incremental Server-Sent-Events frame parser. Feed() accepts arbitrarily-sized
// chunks -- including a chunk that splits a line, or a whole frame, at any byte
// offset, which is what libcurl's write callback does in practice -- and returns
// any frames the chunk completed. Partial line/frame state carries across calls.
class SseParser {
public:
    std::vector<SseFrame> Feed(std::string_view chunk);

private:
    std::string line_buffer_;         // partial last line, carried across Feed() calls
    std::string pending_event_ = "message";
    std::string pending_data_;
    std::string pending_id_;
    bool pending_has_data_ = false;
};

}  // namespace minimal::llm
