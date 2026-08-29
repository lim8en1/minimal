#include <catch2/catch_test_macros.hpp>

#include "minimal/llm/sse_parser.hpp"

using namespace minimal::llm;

namespace {

std::string SampleStream() {
    return
        "event: message_start\n"
        "data: {\"id\":\"1\"}\n"
        "\n"
        "event: content_block_delta\n"
        "data: line one\n"
        "data: line two\n"
        "id: evt-2\n"
        "\n"
        ": this is a comment, ignored\n"
        "data: no explicit event type\n"
        "\n";
}

void CheckFrames(const std::vector<SseFrame>& frames) {
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].event == "message_start");
    CHECK(frames[0].data == "{\"id\":\"1\"}");
    CHECK(frames[1].event == "content_block_delta");
    CHECK(frames[1].data == "line one\nline two");
    CHECK(frames[1].id == "evt-2");
    CHECK(frames[2].event == "message");  // default when no "event:" field was sent
    CHECK(frames[2].data == "no explicit event type");
}

}  // namespace

TEST_CASE("SseParser parses a stream fed in one shot", "[sse]") {
    SseParser parser;
    CheckFrames(parser.Feed(SampleStream()));
}

TEST_CASE("SseParser reassembles frames identically regardless of split point", "[sse]") {
    const std::string stream = SampleStream();

    for (std::size_t split = 0; split <= stream.size(); ++split) {
        SseParser parser;
        auto frames = parser.Feed(std::string_view(stream).substr(0, split));
        auto rest = parser.Feed(std::string_view(stream).substr(split));
        frames.insert(frames.end(), rest.begin(), rest.end());
        CheckFrames(frames);
    }
}

TEST_CASE("SseParser handles CRLF line endings", "[sse]") {
    SseParser parser;
    auto frames = parser.Feed("event: ping\r\ndata: hi\r\n\r\n");
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].event == "ping");
    CHECK(frames[0].data == "hi");
}
