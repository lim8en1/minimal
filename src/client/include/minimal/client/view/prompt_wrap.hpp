#pragma once

// Soft-wrap geometry for the prompt editor (prompt_editor.cpp). Pure std -- no
// FTXUI -- so the wrapping rules are unit-testable without a screen, the same
// tier split key_event_parser uses.
//
// WHY THIS EXISTS: FTXUI 7's Input renders one terminal row per LOGICAL line
// (it splits content at '\n' and nothing else), so a long single-line prompt
// scrolled horizontally and could not be reviewed before sending. The editor
// now renders its own DOM from these visual lines instead of Input's,
// wrapping each logical line word-aware to the terminal width.
//
// TILING INVARIANT: the visual lines' byte ranges [begin, end) cover the
// whole content, every byte in exactly one visual line -- except the '\n'
// separator bytes between logical lines, which no row shows. Cursor mapping
// is therefore plain containment with no gap cases. Trailing spaces a break
// leaves at a row end are kept on that row: invisible there, and keeping them
// contiguous is part of the invariant.

#include <cstddef>
#include <string>
#include <vector>

namespace minimal::client {

// One rendered row of the wrapped prompt.
struct PromptVisualLine {
    std::string text;       // the row's UTF-8 text
    std::size_t begin = 0;  // byte offset of the row's first byte in the content
    std::size_t end = 0;    // one past the row's last byte
};

struct PromptWrapResult {
    std::vector<PromptVisualLine> lines;
    std::size_t cursor_row = 0;  // index into lines
    std::size_t cursor_col = 0;  // display columns from the row's start
};

// Word-aware greedy wrap to `width` display columns (UTF-8 aware: CJK/emoji
// count 2, combining marks 0 -- see utf8.hpp). A row breaks before the word
// that would overflow it; a word wider than a whole row hard-breaks at the
// column limit. `cursor_position` is a BYTE offset (FTXUI's InputOption::
// cursor_position semantics) and always lands on the row whose range contains
// it -- exactly at a break boundary it stays on the earlier row, at its end,
// the way FTXUI's Input keeps a cursor at a logical line's end on that line.
PromptWrapResult WrapPromptContent(const std::string& content, std::size_t cursor_position,
                                   std::size_t width);

}  // namespace minimal::client
