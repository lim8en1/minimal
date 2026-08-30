#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>

namespace minimal::client {

// Renders the assistant message currently streaming in. md4c has no incremental
// parse API and CANNOT soundly parse partial input -- `**bold` cannot be
// interpreted before its closing `**` arrives, so chunk-parsing would produce
// wrong output, not just slower output. So the whole accumulated buffer is
// re-parsed, but at most once per kReparseDebounce; deltas accumulate as a pure
// string append between reparses. Finalize() always reparses once, replacing
// artifacts from mid-stream unclosed spans.
class StreamingMarkdownView {
public:
    static constexpr auto kReparseDebounce = std::chrono::milliseconds(120);

    // `render` re-parses full markdown text into an Element -- in production,
    // MarkdownRenderer::Render. Taken as a function so the debounce state
    // machine is testable without FTXUI assertions or md4c.
    // `clock` is the test seam for time.
    explicit StreamingMarkdownView(
        std::function<ftxui::Element(std::string_view)> render,
        std::function<std::chrono::steady_clock::time_point()> clock =
            std::chrono::steady_clock::now);

    // O(1) amortized, per TextDeltaEvent.
    void AppendDelta(std::string_view text);

    // ContentBlockStop/MessageStop. One authoritative reparse.
    void Finalize();

    // Re-parses only if !finalized_ && elapsed >= debounce (or nothing has been
    // rendered yet). Cheap between reparses: returns the cached element.
    ftxui::Element Render() const;

    // Visible for tests.
    bool finalized() const { return finalized_; }

private:
    std::function<ftxui::Element(std::string_view)> render_;
    std::function<std::chrono::steady_clock::time_point()> clock_;
    std::string accumulated_text_;
    mutable ftxui::Element cached_render_;
    mutable std::size_t rendered_length_ = 0;  // dirty check: skip reparsing unchanged text
    mutable std::chrono::steady_clock::time_point last_reparse_{};
    mutable bool has_rendered_ = false;
    bool finalized_ = false;
};

}  // namespace minimal::client
