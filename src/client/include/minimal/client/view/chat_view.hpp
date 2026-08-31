#pragma once

// The chat view turns ChatRows (chat_model.hpp -- the tested data plane) into
// Elements. Row caching: markdown re-parsing every row every frame would burn
// CPU on each streamed delta, so rows are rebuilt only when the transcript
// version changes; the in-flight block goes through StreamingMarkdownView's
// own debounce. Scrollback is a bounded WINDOW -- a 512k-1M-token history is
// never fully materialized, which is the acceptance criterion, not a nicety.
//
// Per ARCHITECTURE.md "Message-kind rendering":
//   UserText   verbatim styled text -- NOT markdown
//   Attachment collapsed one-liner (path header)
//   Agent      markdown
//   Thinking   collapsed, dimmed
//   Tool       collapsible one-liner + expandable body
//   System     muted, distinctly prefixed (harness, not model)

#include <ftxui/dom/elements.hpp>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/render/markdown_renderer.hpp"
#include "minimal/client/render/streaming_markdown_view.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

class ChatView {
public:
    ChatView(const Theme& theme, const MarkdownRenderer& renderer);

    // `streaming` is the tab's own view; its debounce decides when to re-parse.
    ftxui::Element Render(const SessionState::Snapshot& snapshot, StreamingMarkdownView& streaming);

private:
    void RebuildRows(const SessionState::Snapshot& snapshot);

    const Theme& theme_;
    const MarkdownRenderer& renderer_;
    uint64_t rendered_version_ = 0;   // transcript_version these rows came from
    std::vector<ChatRow> rows_;
    std::vector<ftxui::Elements> rendered_rows_;  // per-row element cache
};

}  // namespace minimal::client
