#include "minimal/client/render/streaming_markdown_view.hpp"

#include <utility>

#include "minimal/common/logger.hpp"

namespace minimal::client {

StreamingMarkdownView::StreamingMarkdownView(
    std::function<ftxui::Element(std::string_view)> render,
    std::function<std::chrono::steady_clock::time_point()> clock)
    : render_(std::move(render)), clock_(std::move(clock)) {}

void StreamingMarkdownView::AppendDelta(std::string_view text) {
    // Text after Finalize() can never render: Render() returns the frozen
    // cached element unconditionally once finalized. Log rather than silently
    // swallow -- a host calling Append after Finalize is miswired.
    if (finalized_) {
        if (!text.empty()) {
            common::DefaultLogger().warn(
                "streaming view: AppendDelta after Finalize ({} bytes ignored)", text.size());
        }
        return;
    }
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
    if (!has_rendered_ || (dirty && now - last_reparse_ >= kReparseDebounce)) {
        cached_render_ = render_(accumulated_text_);
        rendered_length_ = accumulated_text_.size();
        last_reparse_ = now;
        has_rendered_ = true;
    }
    return cached_render_;
}

}  // namespace minimal::client
