#pragma once

// The chat view turns ChatRows (chat_model.hpp -- the tested data plane) into
// Elements. Row caching: markdown re-parsing every row every frame would burn
// CPU on each streamed delta, so rows are rebuilt only when the transcript
// version changes; the in-flight block goes through the engine's streaming
// view's own debounce. Scrollback is a bounded WINDOW -- a 512k-1M-token
// history is never fully materialized, which is the acceptance criterion, not
// a nicety.
//
// Per ARCHITECTURE.md "Message-kind rendering" -- presentation is now slotted
// (E5): each row renders through the MessageRendererRegistry (core supplies
// the defaults for every kind a plugin does not claim), and a tool RESULT row
// consults the ToolResultRendererRegistry by tool name FIRST, falling back to
// the default when no renderer matches (the disabled-plugin fallback is a
// fallback, not an error).

#include <ftxui/dom/elements.hpp>

#include <map>
#include <utility>

#include "minimal/client/chat_model.hpp"
#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

class ChatView {
public:
    ChatView(const Theme& theme,
             const plugins::MessageRendererRegistry& message_renderers,
             const plugins::ToolResultRendererRegistry& tool_renderers);

    // `streaming` is the tab's own view; its debounce decides when to re-parse.
    ftxui::Element Render(const SessionState::Snapshot& snapshot,
                          plugins::MarkdownEngine::Stream& streaming);

private:
    void RebuildRows(const SessionState::Snapshot& snapshot);

    const Theme& theme_;
    const plugins::MessageRendererRegistry& message_renderers_;
    const plugins::ToolResultRendererRegistry& tool_renderers_;
    uint64_t rendered_version_ = 0;   // transcript_version these rows came from
    std::string rendered_session_id_; // which tab they came from (see Render)
    // Aligned with the rows of the current snapshot (oldest first).
    std::vector<ftxui::Elements> rendered_rows_;
    // Per-row render cache keyed by (transcript seq, occurrence within that
    // seq). A scrollback page bump (a version change) re-renders ONLY the rows
    // the page added -- pre-R3 every page load re-markdown-rendered the whole
    // window, including every code block. Evicted rows drop out of the map.
    std::map<std::pair<int64_t, int>, ftxui::Elements> row_cache_;
};

}  // namespace minimal::client
