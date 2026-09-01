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

TEST_CASE("a leading UTF-8 BOM is stripped once per stream", "[sse][invariant]") {
    // Without this, the first data line's field name became "\xEF\xBB\xBFdata"
    // -- unknown field, silently ignored -- and the first OpenAI chunk (id,
    // model, role) was dropped from every BOM-prefixed stream.
    SseParser parser;
    std::string bommed = "\xEF\xBB\xBF";
    bommed += "data: {\"first\":true}\n\n";
    bommed += "data: {\"second\":true}\n\n";
    const auto frames = parser.Feed(bommed);
    REQUIRE(frames.size() == 2);
    CHECK(frames[0].data == "{\"first\":true}");
    CHECK(frames[1].data == "{\"second\":true}");
}

TEST_CASE("a bare CR terminates lines, and a CRLF split across chunks does not flush early",
          "[sse][invariant]") {
    // Only '\n' was recognized: a CR-terminated stream never framed at all and
    // died at the 1 MiB cap. And a '\r' that is the LAST byte held must not
    // terminate -- it may be the first half of a CRLF arriving in the next
    // chunk, and treating it as a terminator would let that '\n' flush a
    // partial frame as if it were complete.
    SseParser parser;
    auto frames = parser.Feed("data: {\"a\":true}\r\ndata: {\"b\":true}\r\n\r\n");
    REQUIRE(frames.size() == 1);  // the blank CRLF line ends the frame
    CHECK(frames[0].data == "{\"a\":true}\n{\"b\":true}");

    SseParser split;
    CHECK(split.Feed("data: {\"a\":true}\r").empty());   // '\r' held: may be CRLF's first half
    frames = split.Feed("\ndata: {\"b\":true}\r\n\r\n");
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].data == "{\"a\":true}\n{\"b\":true}");

    SseParser cr_only;
    // A stream TERMINATING on a held '\r' flushes when more data (or the
    // framing the rest of the stream uses) arrives -- the hold exists for the
    // CRLF split, and a mid-stream blank CR line flushes exactly like an LF
    // one.
    frames = cr_only.Feed("event: error\rdata: boom\r\rdata: next\n\n");
    REQUIRE(frames.size() == 2);  // the CR blank line flushes; the tail frames normally
    CHECK(frames[0].event == "error");
    CHECK(frames[0].data == "boom");
    CHECK(frames[1].data == "next");
}

TEST_CASE("an empty event value is the spec's message, and event names reach the frame",
          "[sse]") {
    SseParser parser;
    auto frames = parser.Feed("event:\ndata: x\n\n");
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].event == "message");
}
