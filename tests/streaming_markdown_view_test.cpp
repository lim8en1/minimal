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
    f.Advance(StreamingMarkdownView::kReparseDebounce);
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

    // Finalized views never reparse again, even with time passing.
    f.Advance(std::chrono::seconds(60));
    f.view.Render();
    CHECK(f.renderer->calls == 2);
}

}  // namespace minimal::client
