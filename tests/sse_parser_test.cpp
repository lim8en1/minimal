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

TEST_CASE("SseParser refuses a line that grows past the cap", "[sse][invariant]") {
    // A stream with no newline grew line_buffer_ without bound. Past 1 MiB the
    // stream is broken, and the throw is the fail-closed posture: the
    // provider's catch-and-classify turns it into an LlmError. Fails with the
    // fix reverted -- the Feed below used to grow the buffer by another MiB.
    SseParser parser;
    const std::string mega(1ull << 20, 'x');
    CHECK_NOTHROW(parser.Feed(mega));                       // exactly at the cap
    CHECK_THROWS_AS(parser.Feed(std::string(2, 'y')),       // and past it
                    std::runtime_error);
}

TEST_CASE("SseParser refuses a frame whose data never completes", "[sse][invariant]") {
    // The other unbounded accumulator: data lines without the blank line that
    // completes the frame. Fed until it throws, rather than to an exact byte
    // count, so the test states the property and not the arithmetic.
    SseParser parser;
    const std::string line(1024, 'a');
    bool threw = false;
    try {
        // 2 MiB of data in 1 KiB lines: comfortably past the 1 MiB cap.
        for (int i = 0; i < 2048; ++i) parser.Feed("data: " + line + "\n");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

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
