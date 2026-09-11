#include "minimal/client/view/prompt_wrap.hpp"

#include <algorithm>
#include <string_view>

#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {

constexpr char32_t kSpaceCp = U' ';
constexpr char32_t kTabCp = U'\t';

// Wraps one '\n'-free span of `content` ([begin, end)) into visual lines,
// appended to `out`. Greedy fill with a word-aware break: when a code point
// would overflow the row, break at the last word boundary (the end of the
// whitespace run before the current word) if there is one on this row, else
// hard-break at the code point. The whitespace before a break stays on the
// old row -- invisible there, and it keeps the rows' byte ranges tiling the
// span (see the header invariant).
void WrapLogicalLine(const std::string& content, std::size_t begin, std::size_t end,
                     std::size_t max_cols, std::vector<PromptVisualLine>& out) {
    std::size_t row_begin = begin;
    std::size_t col = 0;
    // Byte offset a word break may land on (the end of the whitespace run
    // preceding the current word). Breaking at the row start is no break at
    // all -- it would re-emit an empty row forever -- hence the > row_begin
    // guard at use time.
    bool has_break = false;
    std::size_t break_at = begin;

    std::size_t i = begin;
    while (i < end) {
        const std::size_t cp_start = i;
        const char32_t cp = Utf8Decode(content, i);
        const std::size_t cp_cols = Utf8CharColumnWidth(cp);

        if (col + cp_cols > max_cols && cp_cols > 0) {
            if (has_break && break_at > row_begin && break_at <= cp_start) {
                out.push_back(
                    {content.substr(row_begin, break_at - row_begin), row_begin, break_at});
                row_begin = break_at;
                col = Utf8ColumnWidth(std::string_view(content).substr(
                    row_begin, cp_start - row_begin));
                has_break = false;
            }
            if (col + cp_cols > max_cols) {
                // The current word alone will not fit on one row (or the word
                // break landed exactly here): hard-break at the limit.
                out.push_back(
                    {content.substr(row_begin, cp_start - row_begin), row_begin, cp_start});
                row_begin = cp_start;
                col = 0;
                has_break = false;
            }
        }

        if (cp == kSpaceCp || cp == kTabCp) {
            has_break = true;
            break_at = i;   // i is past the code point: the break grows with the run
        }
        col += cp_cols;
    }
    out.push_back({content.substr(row_begin, end - row_begin), row_begin, end});
}

}  // namespace

PromptWrapResult WrapPromptContent(const std::string& content, std::size_t cursor_position,
                                   std::size_t width) {
    PromptWrapResult result;
    const std::size_t cursor = std::min(cursor_position, content.size());
    const std::size_t max_cols = std::max<std::size_t>(width, 1);

    // One logical line per '\n' segment -- including a trailing empty one
    // after a final newline, so the caret has a row to sit on there, matching
    // FTXUI's own line split.
    std::size_t logical_begin = 0;
    while (true) {
        const std::size_t newline = content.find('\n', logical_begin);
        const bool last = newline == std::string::npos;
        const std::size_t logical_end = last ? content.size() : newline;
        WrapLogicalLine(content, logical_begin, logical_end, max_cols, result.lines);
        if (last) break;
        logical_begin = newline + 1;
    }
    if (result.lines.empty()) {
        result.lines.push_back({"", 0, 0});   // empty content: one empty row
    }

    // Map the cursor byte onto its row. Ranges tile the content, so a plain
    // containment walk suffices; `cursor <= end` keeps a cursor sitting
    // exactly on a break boundary on the EARLIER row, at its end -- the same
    // placement FTXUI's Input uses for a cursor at a logical line's end.
    for (std::size_t k = 0; k < result.lines.size(); ++k) {
        if (cursor <= result.lines[k].end || k + 1 == result.lines.size()) {
            result.cursor_row = k;
            break;
        }
    }
    const PromptVisualLine& row = result.lines[result.cursor_row];
    result.cursor_col =
        Utf8ColumnWidth(std::string_view(content).substr(row.begin, cursor - row.begin));
    return result;
}

}  // namespace minimal::client
