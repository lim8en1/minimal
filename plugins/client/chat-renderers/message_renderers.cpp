#include "chat_renderers.hpp"

#include <algorithm>
#include <functional>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/sdk/tool_status.hpp"
#include "minimal/client/sdk/policy.hpp"
#include "policy.hpp"

namespace minimal::client::chat_renderers {

using namespace minimal::client::plugins;

using ftxui::Element;

namespace {

using ftxui::Element;
using ftxui::Elements;

// --- Scroll-anchor chunking ----------------------------------------------------
//
// Scrolling itself is line-granular now (ScrollRegion, chat_scroll_region.hpp)
// -- a tall element no longer snaps whole-message. Chunking still earns its
// keep on the RENDER path: the per-row Elements cache is keyed per chunk, so
// a streamed delta re-renders one small piece, not the whole row, and
// markdown re-parses stay bounded. Decompose long content into chunks of at
// most kMaxAnchorLines source lines.

std::vector<std::string> SplitLines(std::string_view body) {
    std::vector<std::string> lines;
    if (body.empty()) return lines;
    for (std::size_t pos = 0; pos < body.size();) {
        const std::size_t newline = body.find('\n', pos);
        std::string_view line = body.substr(pos, newline == std::string_view::npos
                                                      ? std::string_view::npos
                                                      : newline - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.emplace_back(line);
        if (newline == std::string_view::npos) break;
        pos = newline + 1;
    }
    return lines;
}

bool IsBlankLine(std::string_view line) {
    return line.find_first_not_of(" \t") == std::string_view::npos;
}

// A fenced-code marker: 3+ backticks or tildes (up to three leading spaces of
// indent, like CommonMark). `marker` receives the character run itself, so a
// close can be matched against the opener's length and character; `info` the
// rest of the line (the language tag), needed to re-open the fence in the
// continuation chunk with highlighting intact.
bool IsFenceLine(std::string_view line, std::string_view& marker, std::string_view& info) {
    std::size_t indent = 0;
    while (indent < line.size() && indent < 3 && line[indent] == ' ') ++indent;
    const std::string_view body = line.substr(indent);
    if (body.empty()) return false;
    const char c = body.front();
    if (c != '`' && c != '~') return false;
    std::size_t run = 0;
    while (run < body.size() && body[run] == c) ++run;
    if (run < 3) return false;
    marker = body.substr(0, run);
    info = body.substr(run);
    while (!info.empty() && (info.back() == ' ' || info.back() == '\t')) info.remove_suffix(1);
    return true;
}

// One chunk of a markdown row, with enough fence context that the renderer
// can make each chunk parse on its own: a chunk that continues a fence opened
// in an earlier chunk re-opens it (fence_open_line), a chunk that leaves a
// fence open gets it closed (fence_close_line) -- otherwise the fenced code
// would re-render as prose from the split point on.
struct MarkdownBlock {
    std::string text;                  // the chunk's own lines, no trailing newline
    bool starts_in_fence = false;      // continues a fence from the previous chunk
    bool ends_in_fence = false;        // leaves a fence open for the next chunk
    std::string fence_open_line;       // set when starts_in_fence
    std::string fence_close_line;      // set when ends_in_fence
};

std::vector<MarkdownBlock> SplitMarkdownBlocks(std::string_view body) {
    std::vector<MarkdownBlock> blocks;
    std::vector<std::string> cur;
    bool in_fence = false;
    std::string fence_marker;
    std::string fence_info;   // rest of the opening line (the language), for re-opening
    // Fence state carried ACROSS the flush boundary: whether the next chunk
    // starts inside a fence, and what line re-opens it.
    bool carrying = false;
    std::string carrying_open_line;
    const auto flush = [&] {
        if (cur.empty()) return;
        MarkdownBlock block;
        for (std::size_t i = 0; i < cur.size(); ++i) {
            if (i > 0) block.text += '\n';
            block.text += cur[i];
        }
        block.starts_in_fence = carrying;
        block.fence_open_line = carrying_open_line;
        block.ends_in_fence = in_fence;
        if (in_fence) block.fence_close_line = fence_marker;
        blocks.push_back(std::move(block));
        carrying = in_fence;
        carrying_open_line = fence_marker + fence_info;
        cur.clear();
    };
    for (const std::string& line : SplitLines(body)) {
        std::string_view marker;
        std::string_view info;
        if (IsFenceLine(line, marker, info)) {
            if (!in_fence) {
                in_fence = true;
                fence_marker = std::string(marker);
                fence_info = std::string(info);
            } else if (marker.size() >= fence_marker.size() &&
                       marker.substr(0, fence_marker.size()) == fence_marker &&
                       info.empty()) {
                in_fence = false;   // closing marker: same char, >= opener length, no info
            }
        }
        cur.push_back(line);
        if (in_fence) {
            // Inside a fence only the CAP flushes (a blank line in code is
            // content, not a boundary); the chunk is re-fenced at the split.
            if (cur.size() >= kMaxAnchorLines) flush();
        } else if (IsBlankLine(line) || cur.size() >= kMaxAnchorLines) {
            // Paragraph boundary first (a break at a blank line always reads
            // better than a hard one), the cap as the fallback for a run with
            // no blank lines at all.
            flush();
        }
    }
    flush();
    return blocks;
}

// Plain-text chunking: same cap, no fence logic (a tool log line that happens
// to start with ``` is not a fence). Used for bodies that render verbatim.
std::vector<std::string> SplitTextBlocks(std::string_view body) {
    std::vector<std::string> blocks;
    std::string cur;
    std::size_t lines = 0;
    const auto flush = [&] {
        if (lines == 0) return;
        blocks.push_back(std::move(cur));
        cur.clear();
        lines = 0;
    };
    for (const std::string& line : SplitLines(body)) {
        if (!cur.empty()) cur += '\n';
        cur += line;
        ++lines;
        if (IsBlankLine(line) || lines >= kMaxAnchorLines) flush();
    }
    flush();
    return blocks;
}

// --- Core message renderers ---------------------------------------------------
//
// Extracted from ChatView::RebuildRows verbatim; the kind table's semantics
// live here now. Every renderer colors through the theme, never hardcoded.


// The row's own speaker bar ("▎") is drawn by ChatView as a full-height gutter
// now -- a layout concern, like the timestamp -- so these labels are words
// only.
// A large `@`-mention (a 5k-symbol file, say) is unusable inline -- collapsed
// shows only the header (path + size), expanded shows the full framed block,
// capped at kAttachmentExpandedRenderCapChars so an accidentally-expanded
// huge file cannot wedge the renderer (distinct from -- and much larger than
// -- chat_model.hpp's CollapsedByDefault threshold, which only decides
// whether a row STARTS collapsed).

Elements RenderUserAttachmentBlocks(const ChatRow& row, const Theme& theme,
                                    const RowRenderContext& ctx) {
    using namespace ftxui;
    Elements blocks;
    const int lines = static_cast<int>(std::count(row.text.begin(), row.text.end(), '\n')) + 1;
    blocks.push_back(hbox({Disclosure(ctx, theme),
                           text("@" + row.attachment_path) | color(theme.ColorFor("user_message")),
                           text("  " + std::to_string(lines) + " lines") | Muted(theme)}));
    if (!ctx.collapsed) {
        std::string body = row.text;
        bool truncated = false;
        if (body.size() > kAttachmentExpandedRenderCapChars) {
            body = TruncateUtf8(body, kAttachmentExpandedRenderCapChars);
            truncated = true;
        }
        for (auto& chunk : SplitTextBlocks(body)) {
            blocks.push_back(paragraph(std::move(chunk)) |
                             color(theme.ColorFor("system_message")));
        }
        if (truncated) {
            blocks.push_back(text("··· truncated, too large to render in full ···") |
                             color(theme.ColorFor("system_message")));
        }
    }
    return blocks;
}

Element RenderUserAttachment(const ChatRow& row, const Theme& theme,
                             const RowRenderContext& ctx) {
    using namespace ftxui;
    Elements blocks = RenderUserAttachmentBlocks(row, theme, ctx);
    return blocks.size() == 1 ? std::move(blocks[0]) : vbox(std::move(blocks));
}

// The thinking row's header line, shared by the single-Element and
// chunked paths (the chunked one rides it on the first chunk only).
Element ThinkingHeader(const RowRenderContext& ctx, const Theme& theme) {
    using namespace ftxui;
    return hbox({Disclosure(ctx, theme), text("thinking") | color(theme.ColorFor("thinking"))});
}

Elements RenderThinkingBlocks(const ChatRow& row, const Theme& theme, const RowRenderContext& ctx) {
    using namespace ftxui;
    if (ctx.collapsed) {
        return {hbox({ThinkingHeader(ctx, theme),
                      text("  " + std::to_string(row.text.size()) + " chars") | Muted(theme)})};
    }
    Elements blocks;
    const auto chunks = SplitTextBlocks(row.text);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        Element body = paragraph(chunks[i]) | color(theme.ColorFor("thinking"));
        if (i == 0) body = vbox({ThinkingHeader(ctx, theme), std::move(body)});
        blocks.push_back(std::move(body));
    }
    if (blocks.empty()) blocks.push_back(vbox({ThinkingHeader(ctx, theme)}));
    return blocks;
}

Element RenderThinking(const ChatRow& row, const Theme& theme, const RowRenderContext& ctx) {
    using namespace ftxui;
    Elements blocks = RenderThinkingBlocks(row, theme, ctx);
    return blocks.size() == 1 ? std::move(blocks[0]) : vbox(std::move(blocks));
}

Elements RenderToolActivityBlocks(const ChatRow& row, const Theme& theme,
                                  const RowRenderContext& ctx) {
    using namespace ftxui;
    const std::string name = row.tool_name.empty() ? std::string("tool") : row.tool_name;
    // No result in this page yet (in flight, or the window cut between the
    // pair): Pending, rather than an empty-ended entry that read as a broken
    // render.
    const ToolStatus status = row.tool_is_error ? ToolStatus::Failed
                              : row.tool_pending ? ToolStatus::Pending
                                                 : ToolStatus::Ok;
    Elements line{Disclosure(ctx, theme), ToolHeadline(status, name, theme)};
    // Collapsed is ONE line -- the input summary rides the header rather than
    // a row of its own -- so a turn of many calls reads as a compact list.
    if (ctx.collapsed) {
        const std::string summary = SummarizeToolInput(row.tool_input);
        // Shrinks first: the chat view right-aligns the row timestamp after it.
        if (!summary.empty()) line.push_back(text("  " + summary) | Muted(theme) | flex_shrink);
        return {hbox(std::move(line))};
    }

    // Expanded: the header (plus the command/summary line if any) is
    // the first chunk; the RESULT BODY is what can run long, and it gets its
    // own chunks (SplitTextBlocks) -- a 200-line tool log used to be one
    // indivisible scroll anchor.
    Elements first;
    first.push_back(hbox(std::move(line)));
    // The input summary rides the header (pending and result alike): it is
    // what makes "tool <name>" say what was run.
    if (const std::string summary = InputSummary(row.tool_input); !summary.empty()) {
        first.push_back(paragraph(summary) | color(theme.ColorFor("system_message")));
    }
    Elements blocks;
    blocks.push_back(first.size() == 1 ? std::move(first[0]) : vbox(std::move(first)));

    if (!row.text.empty()) {
        // Bounded body: the capped persisted ToolResultBlock is the
        // authoritative record; this is orientation, not an archive.
        std::string body = row.text;
        if (body.size() > kToolResultBodyPreviewBytes) {
            // FORWARD from the tail window's start, not backward from it:
            // rfind(pos) searches toward index 0 and returns the nearest '\n'
            // at-or-before pos, which can sit anywhere earlier in the string
            // (e.g. "exit\n" + a 1 MB line -- the sole '\n' at index 4 kept
            // ~1 MB, not a preview's worth). find(pos) only ever returns an index
            // >= pos, so the kept slice is bounded no matter where newlines
            // fall; no newline inside the window hard-truncates, as before.
            const std::size_t tail_start = body.size() - kToolResultBodyPreviewBytes;
            const auto newline = body.find('\n', tail_start);
            body = "... " + (newline != std::string::npos
                                  ? body.substr(newline + 1)
                                  : body.substr(Utf8CeilToBoundary(body, tail_start)));
        }
        // Line-buffered output ends in '\n', which paragraph() drew as one
        // trailing empty row under every expanded result.
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
        for (auto& chunk : SplitTextBlocks(ExpandTabs(body))) {
            blocks.push_back(paragraph(std::move(chunk)) |
                             color(theme.ColorFor("system_message")));
        }
    }
    return blocks;
}

Element RenderToolActivity(const ChatRow& row, const Theme& theme, const RowRenderContext& ctx) {
    using namespace ftxui;
    Elements blocks = RenderToolActivityBlocks(row, theme, ctx);
    return blocks.size() == 1 ? std::move(blocks[0]) : vbox(std::move(blocks));
}

Element RenderSystem(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    // Muted and prefixed: "the harness told you something" must never read as
    // "the model said something".
    return hbox({text("[minimal] ") | color(theme.ColorFor("system_message")) | bold,
                 paragraph(row.text) | color(theme.ColorFor("system_message"))});
}

Element RenderAgentMarkdown(const ChatRow& row, const Theme& theme,
                            const MarkdownEngine& engine) {
    return engine.Render(row.text, theme);
}

// "── agent build → plan  ·  model anthropic/sonnet-4-5 → openai/gpt-5 ────":
// only the halves that changed, full model refs (the row labels carry the
// short form). A rule, not a message -- nobody said this.
Element RenderSwitch(const ChatRow& row, const Theme& theme) {
    using namespace ftxui;
    const auto unknown_or = [](const std::string& value) {
        return value.empty() ? std::string("?") : value;
    };
    std::string label;
    if (row.switch_from_agent != row.agent) {
        label += "agent " + unknown_or(row.switch_from_agent) + " → " + unknown_or(row.agent);
    }
    if (row.switch_from_model != row.model) {
        if (!label.empty()) label += "  ·  ";
        label += "model " + unknown_or(row.switch_from_model) + " → " + unknown_or(row.model);
    }
    const Color rule = theme.ColorFor("border_unfocused");
    // separatorCharacter, not separator(): inside an hbox a plain separator
    // draws VERTICAL bars; this one fills its (one-row) box with the glyph.
    return hbox({text("── ") | color(rule), text(label) | Muted(theme), text(" "),
                 separatorCharacter("─") | color(rule) | flex});
}

#define MINIMAL_CORE_RENDERER(kind, body)                                       \
    class Core##kind##Renderer final : public MessageRenderer {                 \
    public:                                                                     \
        Element Render(const ChatRow& row, const Theme& theme,                  \
                       [[maybe_unused]] const RowRenderContext& ctx)            \
            const override body                                                \
    }

MINIMAL_CORE_RENDERER(Switch, { return RenderSwitch(row, theme); });

#undef MINIMAL_CORE_RENDERER

// The remaining core kinds split long content into scroll-anchor chunks
// (RenderBlocks), so they are spelled out rather than macro'd: each pairs a
// single-Element Render with a block decomposition of the same content.
class CoreThinkingRenderer final : public MessageRenderer {
public:
    Element Render(const ChatRow& row, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        return RenderThinking(row, theme, ctx);
    }
    // RenderThinkingBlocks IS the chunked layout; both entry points share it.
    Elements RenderBlocks(const ChatRow& row, const Theme& theme,
                          const RowRenderContext& ctx) const override {
        return RenderThinkingBlocks(row, theme, ctx);
    }
};

class CoreToolActivityRenderer final : public MessageRenderer {
public:
    Element Render(const ChatRow& row, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        return RenderToolActivity(row, theme, ctx);
    }
    Elements RenderBlocks(const ChatRow& row, const Theme& theme,
                          const RowRenderContext& ctx) const override {
        // The decomposition IS the render; collapsed comes back as a single
        // one-line block (see RenderToolActivityBlocks).
        return RenderToolActivityBlocks(row, theme, ctx);
    }
};

class CoreUserAttachmentRenderer final : public MessageRenderer {
public:
    Element Render(const ChatRow& row, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        return RenderUserAttachment(row, theme, ctx);
    }
    Elements RenderBlocks(const ChatRow& row, const Theme& theme,
                          const RowRenderContext& ctx) const override {
        return RenderUserAttachmentBlocks(row, theme, ctx);
    }
};

class CoreSystemRenderer final : public MessageRenderer {
public:
    Element Render(const ChatRow& row, const Theme& theme,
                   const RowRenderContext&) const override {
        return RenderSystem(row, theme);
    }
};

// A compaction: the harness replaced the older part of the model's context
// with a summary. Collapsed, one "[minimal]"-styled line with the numbers;
// expanded, the summary itself -- exactly what the model now reads in place
// of those messages -- through the markdown engine (structured summaries are
// "## Section" markdown).
class CoreCompactionRenderer final : public MessageRenderer {
public:
    explicit CoreCompactionRenderer(const MarkdownEngine& engine) : engine_(engine) {}
    Element Render(const ChatRow& row, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        using namespace ftxui;
        Elements parts;
        parts.push_back(hbox({Disclosure(ctx, theme),
                              text("[minimal] ") | color(theme.ColorFor("system_message")) | bold,
                              text(CompactionHeadline(row)) |
                                  color(theme.ColorFor("system_message"))}));
        // A guard's warning is the point of the row: shown collapsed too.
        if (!row.compaction_warning.empty()) {
            parts.push_back(paragraph("  " + row.compaction_warning) |
                            color(theme.ColorForOr("warning", "error")));
        }
        if (!ctx.collapsed && !row.text.empty()) parts.push_back(engine_.Render(row.text, theme));
        return parts.size() == 1 ? std::move(parts.front()) : vbox(std::move(parts));
    }

private:
    const MarkdownEngine& engine_;
};

// A message through the markdown engine: the assistant's, and the user's own
// (rendered once it is sent -- the finished row is cached by the chat view,
// and its raw text is what rewind, editing and history read). The engine
// keeps typed line breaks as line breaks; CommonMark leaves intra-word `_`
// alone, so snake_case stays snake_case.
class CoreMarkdownMessageRenderer final : public MessageRenderer {
public:
    // `label` names the speaker above the first chunk; `role` is the theme
    // role of the label and the prose's base color.
    using Label = std::function<std::string(const ChatRow&)>;
    CoreMarkdownMessageRenderer(const MarkdownEngine& engine, std::string role, Label label)
        : engine_(engine), role_(std::move(role)), label_(std::move(label)) {}

    Element Render(const ChatRow& row, const Theme& theme,
                   const RowRenderContext& ctx) const override {
        using namespace ftxui;
        const Color base = theme.ColorFor(role_);
        // The base color: markdown only styles spans, so without it the prose
        // fell through to the terminal's own foreground.
        Element body = engine_.Render(row.text, theme) | color(base);
        // Later text of the same turn: the label already sits above it.
        if (ctx.continuation) return body;
        // Every message carries its speaker -- a row with no role marker
        // forced the reader to infer it from styling alone.
        return vbox({text(label_(row)) | color(base) | bold, std::move(body)});
    }

    // Long messages decompose into scroll-anchor chunks: the source splits
    // at paragraph boundaries (a long code fence splits too, each piece
    // re-fenced so highlighting survives -- see SplitMarkdownBlocks), and
    // each piece re-renders through the engine. Every chunk parses on its
    // own, so the concatenation of the chunks looks exactly like the
    // unsplit render.
    Elements RenderBlocks(const ChatRow& row, const Theme& theme,
                          const RowRenderContext& ctx) const override {
        using namespace ftxui;
        const Color base = theme.ColorFor(role_);
        const std::string label = ctx.continuation ? std::string() : label_(row);
        Elements blocks;
        for (auto& block : SplitMarkdownBlocks(row.text)) {
            std::string source;
            if (block.starts_in_fence) source += block.fence_open_line + "\n";
            source += block.text;
            if (block.ends_in_fence) source += "\n" + block.fence_close_line;
            Element body = engine_.Render(source, theme) | color(base);
            if (!label.empty() && blocks.empty()) {
                body = vbox({text(label) | color(base) | bold, std::move(body)});
            }
            blocks.push_back(std::move(body));
        }
        // An empty row (or a splitter edge case producing nothing) keeps the
        // plain render rather than vanishing.
        if (blocks.empty()) return {Render(row, theme, ctx)};
        return blocks;
    }

private:
    const MarkdownEngine& engine_;
    std::string role_;
    Label label_;
};

}  // namespace

void RegisterMessageRenderers(MessageRendererRegistry& registry, const MarkdownEngine& engine) {
    // Fallbacks, not claims: a plugin claiming any one kind replaces just
    // that one, with no collision.
    const auto fill = [&registry](ChatRowKind kind, std::unique_ptr<MessageRenderer> renderer) {
        registry.RegisterFallback(kind, std::move(renderer));
    };
    fill(ChatRowKind::UserAttachment, std::make_unique<CoreUserAttachmentRenderer>());
    fill(ChatRowKind::Thinking, std::make_unique<CoreThinkingRenderer>());
    fill(ChatRowKind::ToolActivity, std::make_unique<CoreToolActivityRenderer>());
    fill(ChatRowKind::System, std::make_unique<CoreSystemRenderer>());
    fill(ChatRowKind::Switch, std::make_unique<CoreSwitchRenderer>());
    fill(ChatRowKind::Compaction, std::make_unique<CoreCompactionRenderer>(engine));
    fill(ChatRowKind::AgentMarkdown,
         std::make_unique<CoreMarkdownMessageRenderer>(
             engine, "assistant_message",
             [](const ChatRow& row) { return AssistantLabel(row.agent, row.model); }));
    fill(ChatRowKind::UserText,
         std::make_unique<CoreMarkdownMessageRenderer>(engine, "user_message",
                                                       [](const ChatRow&) { return "you"; }));
}

}  // namespace minimal::client::chat_renderers
