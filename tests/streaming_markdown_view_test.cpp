#include "minimal/client/render/streaming_markdown_view.hpp"

#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace minimal::client {

namespace {

// The render callback is a counter and a recorder; what matters here is WHEN
// re-parsing happens, not what it produces.
struct FakeRenderer {
    int calls = 0;
    std::string last_text;
    ftxui::Element operator()(std::string_view text) {
        ++calls;
        last_text = std::string(text);
        return ftxui::text(std::string(text));
    }
};

struct Fixture {
    std::chrono::steady_clock::time_point now{};
    std::shared_ptr<FakeRenderer> renderer = std::make_shared<FakeRenderer>();
    StreamingMarkdownView view{[renderer = renderer](std::string_view text) {
                                   return (*renderer)(text);
                               },
                               [this] { return now; }};

    void Advance(std::chrono::milliseconds delta) { now += delta; }
};

}  // namespace

TEST_CASE("StreamingMarkdownView renders immediately, then debounces") {
    Fixture f;
    f.view.AppendDelta("hello");
    f.view.Render();
    CHECK(f.renderer->calls == 1);

    // Deltas within the debounce window must not reparse: they arrive per
    // token and parsing is O(buffer).
    f.view.AppendDelta(" world");
    f.Advance(std::chrono::milliseconds(50));
    f.view.Render();
    CHECK(f.renderer->calls == 1);

    // Past the debounce with new text: reparse, and the new text is in it.
    f.Advance(minimal::client::kMarkdownReparseDebounce);
    f.view.Render();
    CHECK(f.renderer->calls == 2);
    CHECK(f.renderer->last_text == "hello world");
}

TEST_CASE("StreamingMarkdownView does not reparse unchanged text") {
    Fixture f;
    f.view.AppendDelta("static");
    f.view.Render();
    CHECK(f.renderer->calls == 1);

    // Time passing without new deltas is not a reason to reparse.
    f.Advance(std::chrono::seconds(10));
    f.view.Render();
    CHECK(f.renderer->calls == 1);
}

TEST_CASE("Finalize reparses authoritatively regardless of the debounce") {
    Fixture f;
    f.view.AppendDelta("**unclosed");
    f.view.Render();
    CHECK(f.renderer->calls == 1);

    f.view.AppendDelta(" still streaming");
    f.view.Finalize();  // no time advance: the debounce must not block this
    CHECK(f.view.finalized());
    CHECK(f.renderer->calls == 2);
    CHECK(f.renderer->last_text == "**unclosed still streaming");

    // Finalized views never reparse again, even with time passing --
    // PROVIDED no further delta arrives (see the next test case for what
    // happens when one does).
    f.Advance(std::chrono::seconds(60));
    f.view.Render();
    CHECK(f.renderer->calls == 2);
}

// A1: a turn has as many content blocks as the model emits (thinking, text,
// a tool call, more text, ...), and Finalize() marks a BLOCK boundary, not
// the turn's end. AppendDelta used to drop input arriving after Finalize()
// (with a warning) and Render() returned the frozen element forever after --
// so the first content_block_stop of a turn froze the live view for
// everything the model streamed afterward, recovering only once the turn
// ended and the post-turn scrollback fetch swapped in the persisted rows.
TEST_CASE("AppendDelta after Finalize resumes the view instead of freezing it") {
    Fixture f;
    f.view.AppendDelta("block one");
    f.view.Finalize();
    CHECK(f.view.finalized());
    CHECK(f.renderer->calls == 1);

    // A later block's text must reach the renderer, not be silently dropped
    // -- eventually: has_rendered_ is already true from the first Finalize,
    // so this delta falls under the ordinary debounce (like any other
    // post-first-render delta), not the "first render is immediate"
    // bypass. Before the fix this never happened AT ALL, however long you
    // waited -- AppendDelta discarded the text outright once finalized_.
    f.view.AppendDelta(" block two");
    CHECK_FALSE(f.view.finalized());
    f.Advance(minimal::client::kMarkdownReparseDebounce);
    f.view.Render();
    CHECK(f.renderer->calls == 2);
    CHECK(f.renderer->last_text == "block one block two");

    // The second block's own Finalize is the authoritative reparse of
    // everything accumulated across BOTH blocks, exactly like the first.
    f.view.Finalize();
    CHECK(f.view.finalized());
    CHECK(f.renderer->calls == 3);
    CHECK(f.renderer->last_text == "block one block two");
}

// B2: md4c reparses the WHOLE accumulated buffer every tick (no incremental
// API), so a fixed 120 ms cadence spends a growing share of every frame
// budget as a message grows. EffectiveDebounce widens the interval instead.
TEST_CASE("EffectiveDebounce holds the floor below the threshold, "
          "scales and clamps above it") {
    using namespace std::chrono_literals;
    CHECK(StreamingMarkdownView::EffectiveDebounce(0) == minimal::client::kMarkdownReparseDebounce);
    CHECK(StreamingMarkdownView::EffectiveDebounce(minimal::client::kMarkdownReparseScaleThresholdBytes) ==
         minimal::client::kMarkdownReparseDebounce);

    // 1 ms per 64 bytes past the threshold.
    const auto over = minimal::client::kMarkdownReparseScaleThresholdBytes + 640;
    CHECK(StreamingMarkdownView::EffectiveDebounce(over) ==
         minimal::client::kMarkdownReparseDebounce + 10ms);

    // Never exceeds the cap, however large the buffer.
    CHECK(StreamingMarkdownView::EffectiveDebounce(10'000'000) ==
         minimal::client::kMaxMarkdownReparseDebounce);
}

// The scaling is WIRED, not just computed: a large buffer's Render() must
// actually wait longer than kReparseDebounce before reparsing again.
TEST_CASE("Render honors the scaled debounce for a large buffer") {
    Fixture f;
    const std::string big(minimal::client::kMarkdownReparseScaleThresholdBytes + 6400, 'x');
    f.view.AppendDelta(big);
    f.view.Render();
    CHECK(f.renderer->calls == 1);

    f.view.AppendDelta("more");
    f.Advance(minimal::client::kMarkdownReparseDebounce);   // the FLOOR: not enough now
    f.view.Render();
    CHECK(f.renderer->calls == 1);

    // 6400 bytes over the threshold -> +100 ms over the floor (1 ms/64 B).
    f.Advance(std::chrono::milliseconds(100));
    f.view.Render();
    CHECK(f.renderer->calls == 2);
}

// B4: PendingReparseHint is a single process-wide flag (see the header), so
// this test owns its own before/after state rather than assuming a fresh
// process -- Catch2 runs every TEST_CASE in one binary.
TEST_CASE("PendingReparseHint is set while a debounced reparse is pending, "
          "cleared once it happens") {
    StreamingMarkdownView::PendingReparseHint().store(false);
    Fixture f;

    // The first-ever render is immediate (no debounce to wait on): no hint.
    f.view.AppendDelta("hello");
    f.view.Render();
    CHECK_FALSE(StreamingMarkdownView::PendingReparseHint().load());

    // A delta lands, but the debounce window has not closed: this is exactly
    // "the pump's own DispatchToTab-driven wake already happened for this
    // delta, and nothing else will ask again once the window closes" --
    // Render() must flag it.
    f.view.AppendDelta(" world");
    f.view.Render();
    CHECK(StreamingMarkdownView::PendingReparseHint().load());

    // Once the debounce closes and Render() actually reparses, the hint
    // clears -- the pump does not need to keep polling forever.
    f.Advance(minimal::client::kMarkdownReparseDebounce);
    f.view.Render();
    CHECK_FALSE(StreamingMarkdownView::PendingReparseHint().load());

    // Finalize() also clears it (it is its own authoritative reparse, same
    // as a debounce-driven one).
    f.view.AppendDelta("!");
    f.view.Render();
    CHECK(StreamingMarkdownView::PendingReparseHint().load());
    f.view.Finalize();
    CHECK_FALSE(StreamingMarkdownView::PendingReparseHint().load());
}

}  // namespace minimal::client
