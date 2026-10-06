#include "streaming_markdown_view.hpp"
#include "minimal/client/policy.hpp"

#include <algorithm>
#include <utility>

namespace minimal::client::markdown_md4c {

std::chrono::milliseconds StreamingMarkdownView::EffectiveDebounce(std::size_t buffer_size) {
    if (buffer_size <= kMarkdownReparseScaleThresholdBytes) return kMarkdownReparseDebounce;
    const auto extra = std::chrono::milliseconds(
        (buffer_size - kMarkdownReparseScaleThresholdBytes) / kMarkdownReparseBytesPerExtraMs);
    return std::min(kMaxMarkdownReparseDebounce, kMarkdownReparseDebounce + extra);
}

StreamingMarkdownView::StreamingMarkdownView(
    std::function<ftxui::Element(std::string_view)> render,
    std::function<std::chrono::steady_clock::time_point()> clock,
    std::function<void(std::chrono::milliseconds)> redraw_after)
    : render_(std::move(render)), clock_(std::move(clock)), redraw_after_(std::move(redraw_after)) {}

void StreamingMarkdownView::AppendDelta(std::string_view text) {
    // Appending after Finalize() is LEGITIMATE, not a miswiring: Finalize
    // marks the end of a CONTENT BLOCK, and a turn has as many blocks as the
    // model emits -- thinking then text, or text then a tool call then more
    // text. This used to drop the input and warn, so the first
    // content_block_stop of a turn froze the live view for the rest of it:
    // every later block's text went to the log instead of the screen, and
    // only the post-turn scrollback swap ever revealed it. Re-arming here
    // resumes debounced reparsing; Finalize() stays the authoritative reparse
    // of everything accumulated so far, it just no longer terminates the view.
    if (finalized_ && !text.empty()) finalized_ = false;
    accumulated_text_.append(text);
    // Deliberately no reparse here: deltas arrive per token and parsing is
    // O(buffer), so reparse timing belongs to Render() alone.
}

void StreamingMarkdownView::Finalize() {
    finalized_ = true;
    cached_render_ = render_(accumulated_text_);
    rendered_length_ = accumulated_text_.size();
    last_reparse_ = clock_();
    has_rendered_ = true;
}

ftxui::Element StreamingMarkdownView::Render() const {
    if (finalized_) return cached_render_;
    const auto now = clock_();
    const bool dirty = accumulated_text_.size() != rendered_length_;
    const auto debounce = EffectiveDebounce(accumulated_text_.size());
    if (!has_rendered_ || (dirty && now - last_reparse_ >= debounce)) {
        cached_render_ = render_(accumulated_text_);
        rendered_length_ = accumulated_text_.size();
        last_reparse_ = now;
        has_rendered_ = true;
    } else if (dirty) {
        // Debounce not closed yet: the pump has nothing else telling it to
        // check back once it does (DispatchToTab's BumpUiVersion already
        // fired for the delta that made this dirty), so ask for one more
        // poll ourselves.
        // Dirty, but the debounce has not closed: ask for the frame that
        // will reparse it.
        if (redraw_after_) {
            redraw_after_(std::chrono::duration_cast<std::chrono::milliseconds>(
                debounce - (now - last_reparse_)));
        }
    }
    return cached_render_;
}

}  // namespace minimal::client::markdown_md4c
