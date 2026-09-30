#include "minimal/client/render/markdown_renderer.hpp"

#include <limits>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#include <md4c.h>

#include "minimal/client/render/syntax_highlighter.hpp"
#include "minimal/client/utf8.hpp"

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
    bool muted = false;
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
    std::size_t code_block_id = 0;  // this document's Nth code fence (1-based)
    std::string code_text;      // raw; highlighted at leave
    unsigned ordered_index = 0; // item number on an OL (0 is a legal start=)
    bool is_ordered = false;    // parent list is MD_BLOCK_OL -- the marker test
    unsigned li_counter = 0;    // next ordered item number (on a parent OL)
    bool is_task = false;
    char task_mark = ' ';
    std::string link_href;      // set while inside MD_SPAN_A
    std::string link_text;      // concatenated text inside it (autolink detect)
    std::string img_src;        // set while inside MD_SPAN_IMG
    // Table state: one entry per finished row, one Element per cell.
    std::vector<Elements> table_rows;
};


// md4c reports entities VERBATIM ("&amp;", "&#38;", "&#x26;" arrive as their
// literal source bytes) and leaves decoding to the consumer (md4c.h). Decode
// the named set models actually emit plus decimal/hex numeric references;
// anything unknown or malformed comes back unchanged -- the old behavior --
// rather than guessed at.
std::string DecodeEntity(std::string_view entity) {
    struct Named {
        std::string_view source;
        const char* utf8;
    };
    static constexpr Named kNamed[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"},
    };
    for (const Named& named : kNamed) {
        if (entity == named.source) return std::string(named.utf8);
    }
    if (entity.size() < 4 || entity.front() != '&' || entity[1] != '#' ||
        entity.back() != ';') {
        return std::string(entity);
    }
    std::string_view digits = entity.substr(2, entity.size() - 3);
    const bool hex = !digits.empty() && (digits.front() == 'x' || digits.front() == 'X');
    if (hex) digits.remove_prefix(1);
    // 8 digits cap both the realistic cases and char32_t overflow: anything
    // longer is malformed by construction.
    if (digits.empty() || digits.size() > 8) return std::string(entity);
    char32_t cp = 0;
    for (const char c : digits) {
        int value;
        if (c >= '0' && c <= '9') {
            value = c - '0';
        } else if (hex && c >= 'a' && c <= 'f') {
            value = c - 'a' + 10;
        } else if (hex && c >= 'A' && c <= 'F') {
            value = c - 'A' + 10;
        } else {
            return std::string(entity);
        }
        cp = cp * (hex ? 16u : 10u) + static_cast<char32_t>(value);
    }
    // Surrogates and out-of-range values would encode to invalid UTF-8 (and 0
    // has no glyph); render them verbatim instead of corrupting the output.
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return std::string(entity);
    }
    std::string out;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

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
    if (style.muted && theme != nullptr) element = element | Muted(*theme);
    return element;
}

// Folds the accumulated runs of one line into one Element.
Element RunsToElement(Elements runs) {
    if (runs.empty()) return ftxui::text("");
    if (runs.size() == 1) return runs.front();
    // flexbox wraps its children to the available width by default, unlike
    // hbox -- this is what lets a source line with no explicit break still
    // reflow instead of clipping at the terminal edge. It lays out pre-built
    // Elements, so each run's own style (bold/italic/code/link/etc, already
    // applied in PushStyledText) survives untouched.
    return ftxui::flexbox(std::move(runs));
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
                const std::string_view lang(d->lang.text, d->lang.size);
                // CanonicalLanguageFor consults the highlighter's own injected
                // language table (vendored + every plugin grammar), unlike the
                // table-less free function, which only ever matches the
                // vendored aliases -- a plugin-only grammar's fence label used
                // to resolve to "" here and never reach the highlighter at all.
                block.code_language = highlighter_ != nullptr
                                          ? highlighter_->CanonicalLanguageFor(lang)
                                          : CanonicalLanguage(lang);
                block.code_block_id = ++next_code_block_id_;
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
                if (list.type == MD_BLOCK_OL) {
                    // is_ordered is the marker test, NOT index!=0: an OL with
                    // start="0" (legal CommonMark) numbered its first item 0,
                    // which the old ordered_index!=0 test rendered as an
                    // unordered bullet.
                    block.is_ordered = true;
                    block.ordered_index = list.li_counter++;
                }
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
                if (theme_ != nullptr) {
                    rendered = rendered | ftxui::color(theme_->ColorFor("border_unfocused"));
                }
                break;
            case MD_BLOCK_TABLE:
                rendered = block.table_rows.empty()
                               ? ftxui::text("")
                               : ftxui::gridbox(std::move(block.table_rows));
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD: {
                // md4c emits the DISTINCT enum MD_BLOCK_TH for a GFM header
                // cell (md4c.h), not MD_BLOCK_TD -- the header cells used to
                // fall to the default arm below and render as empty text.
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
                rendered = RenderInlineBlock(block) | Mute();
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
                // typeset it, but muting separates live math from the prose
                // around it instead of letting it read as ordinary text.
                style.muted = true;
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
                MuteLast(block);
            }
            block.link_href.clear();
        } else if (type == MD_SPAN_IMG && !block.img_src.empty()) {
            PushStyledText(block, "[image: " + block.img_src + "]", InlineStyle{});
            MuteLast(block);
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
                MuteLast(top);
                break;
            case MD_TEXT_ENTITY:
                // md4c hands the entity's SOURCE text through and expects the
                // consumer to decode it (see DecodeEntity). Code blocks are
                // verbatim content -- md4c never reports entities inside one,
                // so the append below is a just-in-case passthrough.
                if (top.type == MD_BLOCK_CODE) {
                    top.code_text.append(value);
                } else if (top.type == MD_BLOCK_HTML) {
                    PushStyledText(top, std::string(value), InlineStyle{});
                    MuteLast(top);
                } else {
                    PushStyledText(top, DecodeEntity(value), CurrentStyle());
                }
                break;
            case MD_TEXT_NULLCHAR: {
                // md4c does NOT do the replacement itself: MD_TEXT_NULLCHAR
                // exists precisely so the CONSUMER can emit U+FFFD (vendored
                // md4c.h: "CommonMark requires replacing NULL character with
                // the replacement char U+FFFD ... this allows caller to do
                // that easily"). The old fall-through with the wrong comment
                // let ftxui's text() drop the byte, so NULs silently vanished.
                static const std::string kReplacementChar = "\xEF\xBF\xBD";  // U+FFFD
                if (top.type == MD_BLOCK_CODE) {
                    top.code_text.append(kReplacementChar);
                } else {
                    PushStyledText(top, kReplacementChar, CurrentStyle());
                }
                break;
            }
            case MD_TEXT_NORMAL:
            case MD_TEXT_LATEXMATH:
                if (top.type == MD_BLOCK_CODE) {
                    top.code_text.append(value);
                } else if (top.type == MD_BLOCK_HTML) {
                    // Raw HTML renders dimmed (see MD_BLOCK_HTML) -- accumulating
                    // into code_text here was a dead write: HTML blocks render
                    // via RenderInlineBlock, which never reads code_text.
                    PushStyledText(top, std::string(value), InlineStyle{});
                    MuteLast(top);
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
        if (text.empty()) {
            block.current_runs.push_back(ApplyStyle(ftxui::text(""), style, theme_));
            return;
        }
        // One run per word (space kept attached to the PRECEDING word, not
        // pushed as its own run, so flexbox's inter-run gap doesn't double
        // it up): RunsToElement's flexbox only wraps between runs, so a
        // whole phrase pushed as a single run -- the old behavior -- could
        // never break mid-phrase and just clipped at the terminal edge.
        std::size_t start = 0;
        while (start < text.size()) {
            const std::size_t space = text.find(' ', start);
            const std::size_t end = (space == std::string::npos) ? text.size() : space + 1;
            block.current_runs.push_back(
                ApplyStyle(ftxui::text(text.substr(start, end - start)), style, theme_));
            start = end;
        }
    }

    // Secondary text in the theme's muted role -- never SGR dim, which
    // terminals render unreadably.
    ftxui::Decorator Mute() const {
        if (theme_ != nullptr) return Muted(*theme_);
        return [](Element e) { return e; };
    }

    void MuteLast(Block& block) {
        if (!block.current_runs.empty()) {
            block.current_runs.back() = block.current_runs.back() | Mute();
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
        // Tabs are expanded BEFORE any path that builds Elements: ftxui's
        // text() drops them, which silently deleted Makefile/YAML-style tab
        // indentation (ExpandCodeTabs, above, for why 4-column stops).
        block.code_text = ExpandTabs(block.code_text, /*strip_cr=*/false);
        Elements lines;
        if (highlighter_ != nullptr && !block.code_language.empty()) {
            lines = highlighter_->Highlight(block.code_text, block.code_language,
                                            block.code_block_id);
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
        // syntax_plain as the block's base color: untagged blocks and the
        // highlighter's plain gaps read as code, not as the surrounding prose
        // (token colors applied inside still win).
        if (theme_ != nullptr) {
            return ftxui::vbox(std::move(indented)) |
                   ftxui::color(theme_->ColorFor("syntax_plain"));
        }
        return ftxui::vbox(std::move(indented));
    }

    Element RenderQuote(Elements children) {
        Elements quoted;
        quoted.reserve(children.size());
        for (Element& child : children) {
            // A bar that fills the child's full height: a one-row "> "
            // text beside a wrapped paragraph marked only its first line.
            quoted.push_back(ftxui::hbox(
                {ftxui::separatorCharacter("│") | Mute(), ftxui::text(" "), std::move(child)}));
        }
        return ftxui::vbox(std::move(quoted));
    }

    Element RenderListItem(Block& block) {
        std::string marker;
        if (block.is_task) {
            marker = std::string("[") + (block.task_mark == ' ' ? " " : "x") + "] ";
        } else if (block.is_ordered) {
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
    // Ordinal position of each code fence within this Render() call, so the
    // highlighter's incremental parse cache can key on the BLOCK, not just
    // the language -- two same-language fences in one message used to share
    // one cache slot and evict each other's incremental base every tick.
    std::size_t next_code_block_id_ = 0;
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
