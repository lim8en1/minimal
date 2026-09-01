#include "minimal/client/render/markdown_renderer.hpp"

#include <limits>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include <md4c.h>

#include "minimal/client/render/syntax_highlighter.hpp"

namespace minimal::client {

namespace {

using ftxui::Element;
using ftxui::Elements;

// Inline decorations carried down the span stack. Applied in a fixed order so
// nesting order never changes the outcome.
struct InlineStyle {
    bool bold = false;
    bool italic = false;
    bool strikethrough = false;
    bool underline = false;
    bool code = false;
    bool dim = false;
};

// A block being accumulated. Inline-bearing blocks (paragraph, heading, table
// cell) collect styled runs into lines; container blocks collect finished
// child elements.
struct Block {
    MD_BLOCKTYPE type;
    // Inline state.
    std::vector<InlineStyle> style_stack;
    Elements current_runs;       // styled runs on the current line
    std::vector<Elements> lines; // completed lines (hard breaks)
    // Container state.
    Elements children;
    // Block parameters.
    unsigned heading_level = 0;
    std::string code_language;  // fence info string, already canonicalized
    std::string code_text;      // raw; highlighted at leave
    unsigned ordered_index = 0; // 0 = unordered bullet
    unsigned li_counter = 0;    // next ordered item number (on a parent OL)
    bool is_task = false;
    char task_mark = ' ';
    std::string link_href;      // set while inside MD_SPAN_A
    std::string link_text;      // concatenated text inside it (autolink detect)
    std::string img_src;        // set while inside MD_SPAN_IMG
    // Table state: one entry per finished row, one Element per cell.
    std::vector<Elements> table_rows;
};

Element ApplyStyle(Element element, const InlineStyle& style, const Theme* theme) {
    if (style.code) {
        const ftxui::Color c =
            theme != nullptr ? theme->ColorFor("syntax_string") : ftxui::Color::Default;
        if (c != ftxui::Color::Default) element = element | ftxui::color(c);
    }
    if (style.bold) element = element | ftxui::bold;
    if (style.italic) element = element | ftxui::italic;
    if (style.strikethrough) element = element | ftxui::strikethrough;
    if (style.underline) element = element | ftxui::underlined;
    if (style.dim) element = element | ftxui::dim;
    return element;
}

// Folds the accumulated runs of one line into one Element.
Element RunsToElement(Elements runs) {
    if (runs.empty()) return ftxui::text("");
    if (runs.size() == 1) return runs.front();
    return ftxui::hbox(std::move(runs));
}

// The inline content of a block: lines stacked vertically.
Element LinesToElement(std::vector<Elements> lines) {
    if (lines.empty()) return ftxui::text("");
    Elements as_elements;
    as_elements.reserve(lines.size());
    for (Elements& line : lines) as_elements.push_back(RunsToElement(std::move(line)));
    if (as_elements.size() == 1) return as_elements.front();
    return ftxui::vbox(std::move(as_elements));
}

class Renderer {
public:
    Renderer(const Theme* theme, const SyntaxHighlighter* highlighter)
        : theme_(theme), highlighter_(highlighter) {
        Block doc{};
        doc.type = MD_BLOCK_DOC;
        stack_.push(std::move(doc));
    }

    Element Finish() { return std::move(finished_doc_); }

    int EnterBlock(MD_BLOCKTYPE type, void* detail) {
        Block block{};
        block.type = type;
        switch (type) {
            case MD_BLOCK_H:
                block.heading_level = static_cast<const MD_BLOCK_H_DETAIL*>(detail)->level;
                break;
            case MD_BLOCK_CODE: {
                const auto* d = static_cast<const MD_BLOCK_CODE_DETAIL*>(detail);
                block.code_language =
                    CanonicalLanguage(std::string_view(d->lang.text, d->lang.size));
                break;
            }
            case MD_BLOCK_OL:
                block.li_counter = static_cast<const MD_BLOCK_OL_DETAIL*>(detail)->start;
                break;
            case MD_BLOCK_LI: {
                const auto* d = static_cast<const MD_BLOCK_LI_DETAIL*>(detail);
                block.is_task = d->is_task != 0;
                block.task_mark = d->task_mark;
                Block& list = stack_.top();
                if (list.type == MD_BLOCK_OL) block.ordered_index = list.li_counter++;
                break;
            }
            default:
                break;
        }
        stack_.push(std::move(block));
        return 0;
    }

    int LeaveBlock(MD_BLOCKTYPE type, void* detail) {
        Block block = std::move(stack_.top());
        stack_.pop();

        // Table structure is composed here rather than through generic child
        // append: cells land in their row (padded, so gridbox columns do not
        // abut), rows in their section, sections in the table.
        if (type == MD_BLOCK_TR) {
            for (Element& cell : block.children) {
                cell = ftxui::hbox({std::move(cell), ftxui::text("  ")});
            }
            stack_.top().table_rows.push_back(std::move(block.children));
            return 0;
        }
        if (type == MD_BLOCK_THEAD || type == MD_BLOCK_TBODY) {
            Block& table = stack_.top();
            for (Elements& row : block.table_rows) table.table_rows.push_back(std::move(row));
            return 0;
        }

        Element rendered;
        switch (type) {
            case MD_BLOCK_DOC:
                finished_doc_ = ComposeDocument(std::move(block.children));
                return 0;  // nothing above the document to append into
            case MD_BLOCK_P:
                rendered = RenderInlineBlock(block);
                break;
            case MD_BLOCK_H:
                rendered = RenderInlineBlock(block) | ftxui::bold;
                if (block.heading_level <= 2) rendered = rendered | ftxui::underlined;
                break;
            case MD_BLOCK_CODE:
                rendered = RenderCode(block);
                break;
            case MD_BLOCK_QUOTE:
                rendered = RenderQuote(std::move(block.children));
                break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL:
                rendered = ftxui::vbox(std::move(block.children));
                break;
            case MD_BLOCK_LI:
                rendered = RenderListItem(block);
                break;
            case MD_BLOCK_HR:
                rendered = ftxui::separator();
                break;
            case MD_BLOCK_TABLE:
                rendered = block.table_rows.empty()
                               ? ftxui::text("")
                               : ftxui::gridbox(std::move(block.table_rows));
                break;
            case MD_BLOCK_TD: {
                // GFM cell alignment (':' colons in the delimiter row): md4c
                // reports it per cell; the first draft ignored it, so every
                // numeric column LLMs love to right-align rendered left.
                const auto* d = static_cast<const MD_BLOCK_TD_DETAIL*>(detail);
                rendered = RenderInlineBlock(block);
                if (type == MD_BLOCK_TH) rendered = rendered | ftxui::bold;
                switch (d->align) {
                    case MD_ALIGN_CENTER: rendered = rendered | ftxui::hcenter; break;
                    case MD_ALIGN_RIGHT: rendered = rendered | ftxui::align_right; break;
                    case MD_ALIGN_LEFT:
                    case MD_ALIGN_DEFAULT:
                        break;
                }
                break;
            }
            case MD_BLOCK_HTML:
                // Raw HTML passes through dimmed; interpreting it is a browser's
                // job, not a terminal's.
                rendered = RenderInlineBlock(block) | ftxui::dim;
                break;
            default:
                // TR/THEAD/TBODY returned above; this only guards future md4c
                // additions.
                rendered = ftxui::text("");
                break;
        }
        stack_.top().children.push_back(std::move(rendered));
        return 0;
    }

    int EnterSpan(MD_SPANTYPE type, void* detail) {
        Block& block = stack_.top();
        InlineStyle style =
            block.style_stack.empty() ? InlineStyle{} : block.style_stack.back();
        switch (type) {
            case MD_SPAN_STRONG:
                style.bold = true;
                break;
            case MD_SPAN_EM:
                style.italic = true;
                break;
            case MD_SPAN_DEL:
                style.strikethrough = true;
                break;
            case MD_SPAN_U:
                style.underline = true;
                break;
            case MD_SPAN_CODE:
                style.code = true;
                break;
            case MD_SPAN_A: {
                const auto* d = static_cast<const MD_SPAN_A_DETAIL*>(detail);
                block.link_href.assign(d->href.text, d->href.size);
                block.link_text.clear();
                style.underline = true;
                break;
            }
            case MD_SPAN_IMG: {
                const auto* d = static_cast<const MD_SPAN_IMG_DETAIL*>(detail);
                block.img_src.assign(d->src.text, d->src.size);
                break;
            }
            case MD_SPAN_LATEXMATH:
            case MD_SPAN_LATEXMATH_DISPLAY:
                // Math-heavy models emit $...$ constantly; a terminal cannot
                // typeset it, but dimming separates live math from the prose
                // around it instead of letting it read as ordinary text.
                style.dim = true;
                break;
            case MD_SPAN_WIKILINK:
                break;
        }
        block.style_stack.push_back(style);
        return 0;
    }

    int LeaveSpan(MD_SPANTYPE type, void*) {
        Block& block = stack_.top();
        if (!block.style_stack.empty()) block.style_stack.pop_back();
        if (type == MD_SPAN_A) {
            // The target is appended only when it adds information: an autolink
            // renders its URL as the text already, and models emit bare URLs
            // constantly.
            if (block.link_text != block.link_href) {
                PushStyledText(block, " <" + block.link_href + ">", InlineStyle{});
                DimLast(block);
            }
            block.link_href.clear();
        } else if (type == MD_SPAN_IMG && !block.img_src.empty()) {
            PushStyledText(block, "[image: " + block.img_src + "]", InlineStyle{});
            DimLast(block);
            block.img_src.clear();
        }
        return 0;
    }

    int Text(MD_TEXTTYPE type, const MD_CHAR* data, MD_SIZE size) {
        const std::string_view value(data, size);
        Block& top = stack_.top();
        switch (type) {
            case MD_TEXT_SOFTBR:
                // A terminal does not reflow an hbox, so a soft break rendered
                // as a space would let long lines clip. Render it as a line
                // break; the source's own wrapping becomes the wrapping.
                top.lines.push_back(std::move(top.current_runs));
                top.current_runs.clear();
                break;
            case MD_TEXT_BR:
                top.lines.push_back(std::move(top.current_runs));
                top.current_runs.clear();
                break;
            case MD_TEXT_CODE:
                // md4c reports code-BLOCK content as MD_TEXT_CODE too ("includes
                // spaces for indentation" -- md4c.h). Only outside a code block
                // is this an inline `code` span's text.
                if (top.type == MD_BLOCK_CODE) {
                    top.code_text.append(value);
                } else {
                    InlineStyle style = CurrentStyle();
                    style.code = true;
                    PushStyledText(top, std::string(value), style);
                }
                break;
            case MD_TEXT_HTML:
                PushStyledText(top, std::string(value), InlineStyle{});
                DimLast(top);
                break;
            case MD_TEXT_ENTITY:
            case MD_TEXT_NORMAL:
            case MD_TEXT_NULLCHAR:  // md4c maps NUL to U+FFFD; render as-is
            case MD_TEXT_LATEXMATH:
                if (top.type == MD_BLOCK_CODE) {
                    top.code_text.append(value);
                } else if (top.type == MD_BLOCK_HTML) {
                    // Raw HTML renders dimmed (see MD_BLOCK_HTML) -- accumulating
                    // into code_text here was a dead write: HTML blocks render
                    // via RenderInlineBlock, which never reads code_text.
                    PushStyledText(top, std::string(value), InlineStyle{});
                    DimLast(top);
                } else {
                    PushStyledText(top, std::string(value), CurrentStyle());
                }
                break;
        }
        return 0;
    }

private:
    InlineStyle CurrentStyle() const {
        const Block& block = stack_.top();
        return block.style_stack.empty() ? InlineStyle{} : block.style_stack.back();
    }

    void PushStyledText(Block& block, std::string text, InlineStyle style) {
        if (!block.link_href.empty()) block.link_text += text;
        block.current_runs.push_back(
            ApplyStyle(ftxui::text(std::move(text)), style, theme_));
    }

    void DimLast(Block& block) {
        if (!block.current_runs.empty()) {
            block.current_runs.back() = block.current_runs.back() | ftxui::dim;
        }
    }

    Element RenderInlineBlock(Block& block) {
        block.lines.push_back(std::move(block.current_runs));
        return LinesToElement(std::move(block.lines));
    }

    Element RenderCode(Block& block) {
        // A trailing newline from the fence is presentation, not content.
        while (!block.code_text.empty() && block.code_text.back() == '\n') {
            block.code_text.pop_back();
        }
        Elements lines;
        if (highlighter_ != nullptr && !block.code_language.empty()) {
            lines = highlighter_->Highlight(block.code_text, block.code_language);
        } else if (block.code_text.empty()) {
            lines.push_back(ftxui::text(""));
        } else {
            size_t cursor = 0;
            while (cursor <= block.code_text.size()) {
                const size_t next = block.code_text.find('\n', cursor);
                if (next == std::string::npos) {
                    lines.push_back(ftxui::text(block.code_text.substr(cursor)));
                    break;
                }
                lines.push_back(ftxui::text(block.code_text.substr(cursor, next - cursor)));
                cursor = next + 1;
            }
        }
        Elements indented;
        indented.reserve(lines.size());
        for (Element& line : lines) {
            indented.push_back(ftxui::hbox({ftxui::text("  "), std::move(line)}));
        }
        return ftxui::vbox(std::move(indented));
    }

    Element RenderQuote(Elements children) {
        Elements quoted;
        quoted.reserve(children.size());
        for (Element& child : children) {
            quoted.push_back(ftxui::hbox({ftxui::text("> ") | ftxui::dim, std::move(child)}));
        }
        return ftxui::vbox(std::move(quoted));
    }

    Element RenderListItem(Block& block) {
        std::string marker;
        if (block.is_task) {
            marker = std::string("[") + (block.task_mark == ' ' ? " " : "x") + "] ";
        } else if (block.ordered_index != 0) {
            marker = std::to_string(block.ordered_index) + ". ";
        } else {
            marker = "- ";
        }
        // Tight lists stream their text DIRECTLY into the LI with no MD_BLOCK_P
        // wrapper (md4c, CommonMark tight-list rule), so the item's own inline
        // content comes first; nested blocks (a loose paragraph, a sublist)
        // follow. The marker prefixes the first; the rest indent under it.
        Elements body;
        if (!block.current_runs.empty() || !block.lines.empty()) {
            body.push_back(RenderInlineBlock(block));
        }
        for (Element& child : block.children) body.push_back(std::move(child));
        if (body.empty()) body.push_back(ftxui::text(""));
        body.front() = ftxui::hbox({ftxui::text(marker), body.front()});
        const std::string indent(marker.size(), ' ');
        for (size_t i = 1; i < body.size(); ++i) {
            body[i] = ftxui::hbox({ftxui::text(indent), body[i]});
        }
        return ftxui::vbox(std::move(body));
    }

    Element ComposeDocument(Elements blocks) {
        // A blank line between blocks: without it paragraphs fuse into one
        // visual mass.
        Elements with_spacing;
        with_spacing.reserve(blocks.size() * 2);
        bool first = true;
        for (Element& block : blocks) {
            if (!first) with_spacing.push_back(ftxui::text(""));
            with_spacing.push_back(std::move(block));
            first = false;
        }
        return ftxui::vbox(std::move(with_spacing));
    }

    const Theme* theme_;
    const SyntaxHighlighter* highlighter_;
    std::stack<Block> stack_;
    Element finished_doc_ = ftxui::text("");
};

int EnterBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->EnterBlock(type, detail);
}
int LeaveBlockCb(MD_BLOCKTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->LeaveBlock(type, detail);
}
int EnterSpanCb(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->EnterSpan(type, detail);
}
int LeaveSpanCb(MD_SPANTYPE type, void* detail, void* userdata) {
    return static_cast<Renderer*>(userdata)->LeaveSpan(type, detail);
}
int TextCb(MD_TEXTTYPE type, const MD_CHAR* data, MD_SIZE size, void* userdata) {
    return static_cast<Renderer*>(userdata)->Text(type, data, size);
}

}  // namespace

MarkdownRenderer::MarkdownRenderer(const Theme* theme, const SyntaxHighlighter* highlighter)
    : theme_(theme), highlighter_(highlighter) {}

ftxui::Element MarkdownRenderer::Render(std::string_view markdown_text) const {
    Renderer renderer(theme_, highlighter_);
    const MD_PARSER parser = {
        0,  // abi_version
        // GITHUB plus LATEXMATHSPANS: $...$ from math-heavy models gets a dim
        // style rather than reading as ordinary text. Divergence from the
        // dialect presets is recorded in ARCHITECTURE.md ("TUI rendering").
        MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS,
        EnterBlockCb,
        LeaveBlockCb,
        EnterSpanCb,
        LeaveSpanCb,
        TextCb,
        nullptr,  // debug
        nullptr,  // syntax
    };
    // md_parse reports allocation failure only; malformed markdown still parses
    // (it is plain text at worst), so there is no error path to surface.
    if (markdown_text.size() > static_cast<std::size_t>(std::numeric_limits<MD_SIZE>::max())) {
        // MD_SIZE is 32-bit: a message beyond 4 GiB cannot reach md_parse in
        // one piece (theoretical for a TUI, but the truncation would be silent).
        return ftxui::text("message too large to render");
    }
    md_parse(markdown_text.data(), static_cast<MD_SIZE>(markdown_text.size()), &parser,
             &renderer);
    return renderer.Finish();
}

}  // namespace minimal::client
