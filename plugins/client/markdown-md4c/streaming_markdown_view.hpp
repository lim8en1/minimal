#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>
#include "minimal/client/sdk/policy.hpp"
#include "policy.hpp"

namespace minimal::client::markdown_md4c {

// Renders the assistant message currently streaming in. md4c has no incremental
// parse API and CANNOT soundly parse partial input -- `**bold` cannot be
// interpreted before its closing `**` arrives, so chunk-parsing would produce
// wrong output, not just slower output. So the whole accumulated buffer is
// re-parsed, but at most once per kMarkdownReparseDebounce; deltas accumulate as a pure
// string append between reparses. Finalize() always reparses once, replacing
// artifacts from mid-stream unclosed spans.
class StreamingMarkdownView {
public:
    // md4c reparses the WHOLE accumulated buffer every tick (no incremental
    // API -- see the class comment), so the cost grows with the message, not
    // just with how often Render() is called: a long, code-heavy reply got
    // progressively janky near its end at a fixed cadence. Past
    // kMarkdownReparseScaleThresholdBytes the debounce grows (EffectiveDebounce), capped at
    // kMaxMarkdownReparseDebounce -- the tail of a long message still updates, just
    // less often, rather than a fixed-rate reparse eating a growing share of
    // every frame budget.

    // Visible for tests: the debounce Render() actually uses for a buffer of
    // `buffer_size` bytes. kMarkdownReparseDebounce below the threshold; grows
    // linearly above it (1 ms per 64 bytes past the threshold), clamped at
    // kMaxMarkdownReparseDebounce.
    static std::chrono::milliseconds EffectiveDebounce(std::size_t buffer_size);

    // `render` re-parses full markdown text into an Element -- in production,
    // MarkdownRenderer::Render. Taken as a function so the debounce state
    // machine is testable without FTXUI assertions or md4c.
    // `clock` is the test seam for time. `redraw_after` (optional) is asked
    // for a frame once the debounce closes, whenever Render() declines to
    // reparse: a burst of deltas that PAUSES wakes the pump once, for the
    // pause's last delta -- if the debounce had not closed by that render,
    // nothing else would bring the next frame, and the tail sat rendered
    // stale until some unrelated event arrived. In the TUI it is the host's
    // HostUi::RequestRedrawAfter.
    explicit StreamingMarkdownView(
        std::function<ftxui::Element(std::string_view)> render,
        std::function<std::chrono::steady_clock::time_point()> clock =
            std::chrono::steady_clock::now,
        std::function<void(std::chrono::milliseconds)> redraw_after = {});

    // O(1) amortized, per TextDeltaEvent. Legal after Finalize(): a turn has
    // as many content blocks as the model emits, and the block boundary is
    // what Finalize() marks -- see the .cpp for the freeze this re-arming
    // fixes.
    void AppendDelta(std::string_view text);

    // ContentBlockStop/MessageStop. One authoritative reparse of everything
    // accumulated so far. NOT terminal: a later AppendDelta re-arms the view.
    void Finalize();

    // Re-parses only if !finalized_ && elapsed >= debounce (or nothing has been
    // rendered yet). Cheap between reparses: returns the cached element.
    ftxui::Element Render() const;

    // Visible for tests. True only between a Finalize() and the next delta.
    bool finalized() const { return finalized_; }

private:
    std::function<ftxui::Element(std::string_view)> render_;
    std::function<std::chrono::steady_clock::time_point()> clock_;
    std::function<void(std::chrono::milliseconds)> redraw_after_;
    std::string accumulated_text_;
    mutable ftxui::Element cached_render_;
    mutable std::size_t rendered_length_ = 0;  // dirty check: skip reparsing unchanged text
    mutable std::chrono::steady_clock::time_point last_reparse_{};
    mutable bool has_rendered_ = false;
    bool finalized_ = false;
};

}  // namespace minimal::client::markdown_md4c
