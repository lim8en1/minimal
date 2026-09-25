// Chat scrolling tests: the line-granular ScrollRegion (chat_scroll_region.hpp)
// and ChatView's line offsets. The scheme this replaced -- one focus() marker
// per render CHUNK under FTXUI's yframe -- had two proven-bad properties this
// file pins down the fixes for:
//   * yframe CENTERS the focused element, so each step overshot to mid-viewport
//     and a chunk taller than the viewport (dense thinking prose wraps far past
//     its 20-source-line cap) leapt several screens per wheel tick;
//   * the row cursor (chat.row_up/chat.row_down) hijacked the focus anchor, so
//     every wheel tick was silently ignored until PageDown cleared the cursor.
//
// Assertions read the rendered frame's "line-NN" sentinels: the visible NN set
// IS the scroll window, so movement, clamping and granularity are all directly
// observable without a terminal.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "ftxui_harness.hpp"

#include "minimal/client/chat_model.hpp"
#include "minimal/client/plugins/core_registrations.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"
#include "minimal/client/view/chat_scroll_region.hpp"
#include "minimal/client/view/chat_view.hpp"

using namespace minimal::client;
using namespace minimal_test;
namespace ftx = ftxui;
namespace v1 = minimal::v1;

namespace {

Theme TestTheme() {
    return Theme::Parse(R"({"name":"test","colors":{}})", ColorDepth::Truecolor);
}

v1::StoredMessage StoredUserText(int64_t seq, const std::string& text) {
    v1::StoredMessage stored;
    stored.set_seq(seq);
    stored.mutable_message()->set_role(v1::ROLE_USER);
    auto* block = stored.mutable_message()->add_content();
    block->mutable_text()->set_text(text);
    return stored;
}

// One message of `count` distinct single-screen lines ("line-00" ...), no
// blank lines: paragraph() maps source lines 1:1 onto screen rows (nothing
// wraps at the test width), so the visible line numbers are an exact scroll
// readout. One row also means one spacer and one label -- a wheel tick's
// 3-line step is measurable against the line numbers alone.
SessionState::Snapshot SnapshotWithNumberedLines(const std::string& session_id,
                                                 uint64_t version, int count) {
    std::string text;
    for (int i = 0; i < count; ++i) {
        if (i > 0) text += '\n';
        char buf[8];
        std::snprintf(buf, sizeof(buf), "line-%02d", i);
        text += buf;
    }
    SessionState::Snapshot snap;
    snap.session_id = session_id;
    snap.transcript_version = version;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::vector<v1::StoredMessage>{StoredUserText(1, text)});
    return snap;
}

// Every "<prefix>-NN" sentinel visible in the frame.
std::set<int> VisibleLines(const std::string& frame, const std::string& prefix = "line-") {
    std::set<int> seen;
    for (std::size_t pos = frame.find(prefix); pos != std::string::npos;
         pos = frame.find(prefix, pos + 1)) {
        const std::string digits = frame.substr(pos + prefix.size(), 2);
        if (digits.size() == 2 && std::isdigit(static_cast<unsigned char>(digits[0])) &&
            std::isdigit(static_cast<unsigned char>(digits[1]))) {
            seen.insert(std::stoi(digits));
        }
    }
    return seen;
}

int LowestVisibleLine(const std::string& frame) {
    const auto seen = VisibleLines(frame);
    return seen.empty() ? -1 : *seen.begin();
}

// The row cursor's gutter bar drawn at a line START (the vscroll_indicator
// draws the same glyph at the frame's right edge, so a raw find() would
// false-positive on any scrolled frame).
bool AnyLineStartsWithCursor(const std::string& frame) {
    std::size_t start = 0;
    while (start <= frame.size()) {
        const std::size_t end = frame.find('\n', start);
        std::string line =
            frame.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // ┃ (U+2503) is multi-byte in UTF-8; compare bytes, not a char literal.
        if (line.rfind("\u2503", 0) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

// The gate (kWheelGateMs) drops wheel ticks closer together than ~30ms; a
// tick spaced past it always applies.
void PassWheelGate() {
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
}

struct ChatViewFixture {
    ChatViewFixture()
        : renderer(&theme, nullptr) {
        plugins::RegisterDefaultMarkdownEngine(engine_registry, renderer);
        plugins::RegisterCoreMessageRenderers(messages);
        plugins::RegisterCoreAgentMarkdownRenderer(messages, *engine_registry.engine);
    }

    Theme theme{TestTheme()};
    MarkdownRenderer renderer;
    plugins::MarkdownEngineRegistry engine_registry;
    plugins::MessageRendererRegistry messages;
    plugins::ToolResultRendererRegistry tool_renderers;
    plugins::DisplayRendererRegistry display_renderers;
};

}  // namespace

// ---------------------------------------------------------------------------
// ScrollRegion (the explicit-offset node)
// ---------------------------------------------------------------------------

TEST_CASE("scroll region: offset 0 pins to the newest content") {
    auto state = std::make_shared<ScrollState>();
    ftx::Elements lines;
    for (int i = 0; i < 40; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "line-%02d", i);
        lines.push_back(ftx::text(buf));
    }
    auto element = ScrollRegion(ftx::vbox(std::move(lines)), state);

    ftx::Screen screen(80, 10);
    ftx::Render(screen, element);
    CHECK(state->applied_offset_from_bottom == 0);
    CHECK(state->content_height == 40);
    CHECK(state->viewport_height == 10);
    const auto visible = VisibleLines(screen.ToString());
    REQUIRE(visible.size() == 10);
    CHECK(*visible.begin() == 30);   // newest (bottom) content on screen
}

TEST_CASE("scroll region: the desired offset moves exactly that many lines") {
    auto state = std::make_shared<ScrollState>();
    ftx::Elements lines;
    for (int i = 0; i < 40; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "line-%02d", i);
        lines.push_back(ftx::text(buf));
    }
    auto element = ScrollRegion(ftx::vbox(std::move(lines)), state);

    state->desired_offset_from_bottom = 5;
    ftx::Screen screen(80, 10);
    ftx::Render(screen, element);
    CHECK(state->applied_offset_from_bottom == 5);
    const auto visible = VisibleLines(screen.ToString());
    REQUIRE(visible.size() == 10);
    CHECK(*visible.begin() == 25);   // 30 - 5: line-granular, no centering overshoot
}

TEST_CASE("scroll region: the offset clamps against the real content") {
    auto state = std::make_shared<ScrollState>();
    ftx::Elements lines;
    for (int i = 0; i < 40; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "line-%02d", i);
        lines.push_back(ftx::text(buf));
    }
    auto element = ScrollRegion(ftx::vbox(std::move(lines)), state);

    state->desired_offset_from_bottom = 1000;
    ftx::Screen screen(80, 10);
    ftx::Render(screen, element);
    CHECK(state->applied_offset_from_bottom == 30);   // 40 - 10: the top
    CHECK(*VisibleLines(screen.ToString()).begin() == 0);

    // A SHRUNK window (page trim) must not wedge the offset past the new top.
    state->desired_offset_from_bottom = 30;
    ftx::Elements few;
    for (int i = 0; i < 8; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "line-%02d", i);
        few.push_back(ftx::text(buf));
    }
    auto small = ScrollRegion(ftx::vbox(std::move(few)), state);
    ftx::Screen screen2(80, 10);
    ftx::Render(screen2, small);
    CHECK(state->applied_offset_from_bottom == 0);
    CHECK(state->content_height == 10);   // stretched to the viewport, not 8
}

// The whole point of the rewrite: ONE element far taller than the viewport
// (what an unchunked multi-line thinking block renders as) scrolls line by
// line -- there is no per-element anchor to leap over.
TEST_CASE("scroll region: a single element taller than the viewport scrolls in lines") {
    auto state = std::make_shared<ScrollState>();
    // One 40-line element -- paragraph() of a blank-line-free block, or a
    // plugin renderer that never opted into chunking.
    std::string body;
    for (int i = 0; i < 40; ++i) {
        if (i > 0) body += '\n';
        char buf[8];
        std::snprintf(buf, sizeof(buf), "line-%02d", i);
        body += buf;
    }
    auto element = ScrollRegion(ftx::vbox(ftx::paragraph(body)), state);

    state->desired_offset_from_bottom = 1;
    ftx::Screen screen(80, 10);
    ftx::Render(screen, element);
    const auto visible = VisibleLines(screen.ToString());
    REQUIRE(visible.size() == 10);
    CHECK(*visible.begin() == 29);   // one line, not one 40-line anchor
}

// ---------------------------------------------------------------------------
// ChatView wheel scrolling
// ---------------------------------------------------------------------------

TEST_CASE("chat view: one wheel tick scrolls exactly kWheelLinesPerTick lines") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    const auto snap = SnapshotWithNumberedLines("s", 1, 60);

    const auto pinned = RenderElement(view.Render(snap), 80, 24);
    const int pinned_top = LowestVisibleLine(pinned);
    CHECK(pinned_top == 37);   // content line 39 (spacer) minus 23 lines above

    PassWheelGate();
    view.ScrollWheelBy(1);
    const auto after_one = RenderElement(view.Render(snap), 80, 24);
    CHECK(LowestVisibleLine(after_one) == pinned_top - 3);

    PassWheelGate();
    view.ScrollWheelBy(1);
    const auto after_two = RenderElement(view.Render(snap), 80, 24);
    CHECK(LowestVisibleLine(after_two) == pinned_top - 6);
}

TEST_CASE("chat view: the wheel gate drops ticks inside the gate window") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    const auto snap = SnapshotWithNumberedLines("s", 1, 60);

    (void)RenderElement(view.Render(snap), 80, 24);
    PassWheelGate();
    view.ScrollWheelBy(1);
    const auto first = RenderElement(view.Render(snap), 80, 24);

    view.ScrollWheelBy(1);   // momentum burst: dropped, no state change
    const auto second = RenderElement(view.Render(snap), 80, 24);
    CHECK(second == first);
}

TEST_CASE("chat view: scrolling clamps at both ends with no overshoot to burn") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    const auto snap = SnapshotWithNumberedLines("s", 1, 60);

    (void)RenderElement(view.Render(snap), 80, 24);
    view.ScrollBy(1000);   // keyboard steps: far past the top
    const auto top = RenderElement(view.Render(snap), 80, 24);
    CHECK(LowestVisibleLine(top) == 0);
    CHECK(view.IsScrolledAway());

    // The stored offset was CLAMPED at the top render, not ballooned: one
    // step back down lands on the newest content immediately.
    view.ScrollBy(-1000);
    const auto bottom = RenderElement(view.Render(snap), 80, 24);
    CHECK(LowestVisibleLine(bottom) == 37);
    CHECK_FALSE(view.IsScrolledAway());
    CHECK(VisibleLines(bottom).count(59) == 1);   // newest line on screen
}

// The old bug: alt+k/alt+j set a row cursor that silently swallowed every
// wheel tick. Now the first tick retires the cursor into a line offset (the
// wheel takes over from exactly where the cursor sat) and keeps scrolling.
TEST_CASE("chat view: a wheel tick retires the row cursor and takes over scrolling") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    const auto snap = SnapshotWithNumberedLines("s", 1, 60);

    const auto pinned = RenderElement(view.Render(snap), 80, 24);
    CHECK(LowestVisibleLine(pinned) == 37);   // pinned: newest content on screen

    view.MoveRowFocus(1);   // cursor on the one row (taller than the viewport)
    const auto cursor_frame = RenderElement(view.Render(snap), 80, 24);
    // Reveal pulls the row's TOP edge (its label line) to the viewport top.
    CHECK(AnyLineStartsWithCursor(cursor_frame));
    CHECK(LowestVisibleLine(cursor_frame) == 0);
    CHECK(view.IsScrolledAway());

    PassWheelGate();
    view.ScrollWheelBy(-1);   // retires the cursor, then scrolls 3 lines
    const auto after = RenderElement(view.Render(snap), 80, 24);
    CHECK_FALSE(AnyLineStartsWithCursor(after));
    CHECK(LowestVisibleLine(after) == 2);   // moved 3 lines from the reveal
    CHECK(view.IsScrolledAway());

    view.ScrollToBottom();
    const auto bottom = RenderElement(view.Render(snap), 80, 24);
    CHECK_FALSE(view.IsScrolledAway());
    CHECK(LowestVisibleLine(bottom) == 37);
}

// The cursor reveal must be judged against the VIEWPORT, not against
// absolute content coordinates: a row fully above the viewport (e.g. the
// second-newest row, where alt+k lands from a resting cursor) must pull the
// view UP to itself. Judged against the viewport HEIGHT instead, an
// above-viewport row always read "below the fold" and the reveal slammed to
// the bottom -- the exact bug the pty smoke run caught.
TEST_CASE("chat view: the row cursor reveals rows above and below the viewport") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    // Two 40-line messages: "aa-NN" (oldest) then "bb-NN" (newest).
    // scrollback_page is DESCENDING by seq (newest first), so bb goes in
    // with the higher seq but first position.
    std::vector<v1::StoredMessage> page;
    int64_t seq = 2;
    for (const auto* prefix : {"bb", "aa"}) {   // newest at index 0
        std::string text;
        for (int i = 0; i < 40; ++i) {
            if (i > 0) text += '\n';
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%s-%02d", prefix, i);
            text += buf;
        }
        page.push_back(StoredUserText(seq--, text));
    }
    SessionState::Snapshot snap;
    snap.session_id = "reveal";
    snap.transcript_version = 1;
    snap.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(
        std::move(page));

    const auto pinned = RenderElement(view.Render(snap), 80, 24);
    CHECK(VisibleLines(pinned, "bb-").count(39) == 1);   // newest content on screen

    // alt+k: cursor onto the OLDER row, fully above the viewport -> the view
    // jumps UP to its top, which carries the cursor marker.
    view.MoveRowFocus(1);
    const auto revealed = RenderElement(view.Render(snap), 80, 24);
    CHECK(AnyLineStartsWithCursor(revealed));
    const auto aa = VisibleLines(revealed, "aa-");
    CHECK(aa.count(0) == 1);   // aa-00: the older row's top on screen
    CHECK(aa.lower_bound(23) == aa.end());   // no aa-23+ visible below it

    // alt+j from there: cursor onto the NEWEST row, fully below -> the view
    // jumps DOWN so the row's head sits at the viewport's bottom edge.
    view.MoveRowFocus(-1);
    const auto back_down = RenderElement(view.Render(snap), 80, 24);
    // The row's HEAD (its label line) sits at the viewport's bottom edge.
    CHECK(AnyLineStartsWithCursor(back_down));
    CHECK(back_down.find("you") != std::string::npos);   // the head line is the label
    const auto bb = VisibleLines(back_down, "bb-");
    CHECK(bb.count(0) == 0);    // the body starts below the fold
    CHECK(bb.count(39) == 0);
}

TEST_CASE("chat view: short content never scrolls and reports pinned") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    const auto snap = SnapshotWithNumberedLines("s", 1, 3);

    const auto frame = RenderElement(view.Render(snap), 80, 24);
    const auto visible = VisibleLines(frame);
    CHECK(visible.size() == 3);   // nothing clipped
    view.ScrollBy(10);
    (void)RenderElement(view.Render(snap), 80, 24);
    CHECK_FALSE(view.IsScrolledAway());
}

// Regression: the scroll position was an offset from the BOTTOM only, so a
// reader parked mid-transcript slid along with every line landing below.
TEST_CASE("chat view: scrolled away, content arriving below does not move the view") {
    ChatViewFixture fx;
    ChatView view(fx.theme, fx.messages, fx.tool_renderers, fx.display_renderers);
    const auto snap = SnapshotWithNumberedLines("s", 1, 60);

    (void)RenderElement(view.Render(snap), 80, 24);
    view.ScrollBy(1);
    const auto parked = RenderElement(view.Render(snap), 80, 24);
    const int parked_top = LowestVisibleLine(parked);
    REQUIRE(view.IsScrolledAway());
    // One frame with nothing new: the anchor is picked, nothing moves.
    CHECK(LowestVisibleLine(RenderElement(view.Render(snap), 80, 24)) == parked_top);
    CHECK_FALSE(view.HasUnseenBelow());

    // A new 30-line message lands below.
    auto grown = snap;
    grown.transcript_version = 2;
    std::string more;
    for (int i = 0; i < 30; ++i) more += (i ? "\n" : "") + std::string("more ") + std::to_string(i);
    auto page = *snap.scrollback_page;
    page.insert(page.begin(), StoredUserText(2, more));
    grown.scrollback_page = std::make_shared<const std::vector<v1::StoredMessage>>(page);
    const auto after = RenderElement(view.Render(grown), 80, 24);
    CHECK(LowestVisibleLine(after) == parked_top);
    CHECK(view.IsScrolledAway());
    CHECK(view.HasUnseenBelow());   // the frame's "new output below" hint

    view.ScrollToBottom();
    (void)RenderElement(view.Render(grown), 80, 24);
    CHECK_FALSE(view.HasUnseenBelow());
}
