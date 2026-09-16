#include "minimal/client/view/filterable_menu.hpp"

#include <algorithm>
#include <optional>

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/utf8.hpp"

namespace minimal::client {
namespace {

// "#rrggbb" -> Color, case-insensitive, tolerant: anything malformed yields
// nullopt (theme default) rather than an exception -- a picker row's cosmetics
// must never break rendering. Downsampled to the theme's depth like every
// theme role: a preset's raw truecolor used to reach 256/16-color terminals.
std::optional<ftxui::Color> RowColor(const std::string& value, ColorDepth depth) {
    if (value.size() != 7 || value.front() != '#') return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    // Each nibble is checked BEFORE combining: with only the combined results
    // guarded, a valid high nibble and an invalid low one (nibble(hi)*16 +
    // nibble(lo) == hi*16 - 1) could produce a small plausible value -- e.g.
    // "#1g0000" would decode as 0x0f rather than being rejected.
    const int hi_hi = nibble(value[1]);
    const int hi_lo = nibble(value[2]);
    const int mid_hi = nibble(value[3]);
    const int mid_lo = nibble(value[4]);
    const int lo_hi = nibble(value[5]);
    const int lo_lo = nibble(value[6]);
    if (hi_hi < 0 || hi_lo < 0 || mid_hi < 0 || mid_lo < 0 || lo_hi < 0 || lo_lo < 0) {
        return std::nullopt;
    }
    const int hi = hi_hi * 16 + hi_lo;
    const int mid = mid_hi * 16 + mid_lo;
    const int lo = lo_hi * 16 + lo_lo;
    const ftxui::Color c = Downsample(static_cast<std::uint8_t>(hi), static_cast<std::uint8_t>(mid),
                                      static_cast<std::uint8_t>(lo), depth);
    // ColorDepth::None: no tint, so the window's own color still applies.
    if (c == ftxui::Color::Default) return std::nullopt;
    return c;
}

}  // namespace

// The index the cursor should occupy, given `from` and the direction the user
// last moved (+1 down, -1 up): `from` itself when it is selectable (in bounds,
// non-header); otherwise the nearest selectable row scanning in `direction`,
// falling back to the opposite side when that side is exhausted, and -1 when
// the list has no selectable row at all (or is empty). Pure so the
// header-skipping rules are directly testable (tests/filterable_menu_test.cpp).
int ResolveSelectableRow(const std::vector<MenuRow>& rows, int from, int direction) {
    // In bounds and selectable: stay.
    if (from >= 0 && from < static_cast<int>(rows.size()) && !rows[from].header) {
        return from;
    }
    // Scan from `from` in the travel direction, then the opposite side (a
    // down-past-the-end filter rebuild lands on the last selectable row, not
    // the first), so the cursor always has a home when one exists. Each scan
    // starts clamped into range: `from` itself may sit outside the list
    // (Menu's clamp runs after its move; a rebuild can shrink the list first).
    const int size = static_cast<int>(rows.size());
    for (int probe : {direction, -direction}) {
        const int start = probe > 0 ? std::max(from, 0) : std::min(from, size - 1);
        for (int i = start; i >= 0 && i < size; i += probe) {
            if (!rows[i].header) return i;
        }
    }
    return -1;
}

ftxui::Component MakeFilterableMenuModal(
    std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
    const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks, const Theme& theme) {
    using namespace ftxui;

    // ALL mutable state lives in one shared block owned by every closure
    // below. The historical bug this guards against (session_picker.cpp's
    // original form): a stack-local `selected` and a by-value-captured
    // index froze the highlight at row 0 regardless of where the cursor
    // actually was -- the closures outlive this factory call, so everything
    // they touch must too.
    struct MenuState {
        std::vector<std::string> labels;   // Menu's own label list, rebuilt on filter change
        std::vector<MenuRow> rows;          // parallel to labels
        int selected = 0;
        int direction = 1;   // last travel direction: +1 down, -1 up (see ResolveSelectableRow)
        std::string filter;
    };
    auto state = std::make_shared<MenuState>();

    auto rebuild = [state, build_rows] {
        state->rows = build_rows(state->filter);
        state->labels.clear();
        state->labels.reserve(state->rows.size());
        for (const auto& row : state->rows) state->labels.push_back(row.label);
        const int last = static_cast<int>(state->labels.size()) - 1;
        state->selected = std::clamp(state->selected, 0, std::max(last, 0));
        state->selected =
            ResolveSelectableRow(state->rows, state->selected, state->direction);
    };
    rebuild();

    // Header rows (the model picker's provider groups) get their own entry
    // style: bold, flush-left, never cursor-prefixed or highlighted. Normal rows
    // replicate FTXUI's DefaultOptionTransform plus one indent level, so the
    // two tiers read visually. The transform reads state->rows by index --
    // the same shared block every closure here uses, so it can never outlive
    // or diverge from what Menu itself is displaying.
    MenuOption menu_options;
    menu_options.entries_option.transform =
        [state, depth = theme.depth(), selection = SelectionStyle(theme)](
            const EntryState& entry) -> Element {
        const bool is_header = entry.index >= 0 &&
                               entry.index < static_cast<int>(state->rows.size()) &&
                               state->rows[entry.index].header;
        if (is_header) {
            return text(state->rows[entry.index].label) | bold;
        }
        // "❯ " -- the same cursor glyph the permission modal's answer list
        // uses, so every list in the app marks its selection one way.
        std::string label =
            (std::string(entry.active ? "❯ " : "  ") + "  ") + state->rows[entry.index].label;
        Element e = text(std::move(label));
        if (const std::optional<ftxui::Color> tint =
                RowColor(state->rows[entry.index].color, depth)) {
            e = e | color(*tint);
        }
        // Applied OUTSIDE the tint, so a preset's own color survives focus.
        if (entry.focused) e = e | selection;
        if (entry.active) e = e | bold;
        return e;
    };

    auto menu = Menu(&state->labels, &state->selected, menu_options);

    auto interactive = CatchEvent(menu, [state, callbacks, rebuild](const Event& event) {
        if (event == Event::Return) {
            if (!state->rows.empty() && !state->rows[state->selected].header &&
                callbacks.on_select) {
                callbacks.on_select(state->rows[state->selected]);
            }
            return true;   // consumed either way: no double-submit on empty list
        }
        if (event == Event::Escape) {
            if (callbacks.on_close) callbacks.on_close();
            return true;
        }
        if (event == Event::Backspace) {
            if (!state->filter.empty()) {
                // Grapheme-aware: one press removes one user-perceived
                // character, so a ZWJ-emoji filter entry deletes as one
                // glyph instead of shedding components one keypress at a
                // time.
                const std::string_view popped =
                    Utf8PopGrapheme(state->filter);
                state->filter.assign(popped);
                rebuild();
            }
            return true;
        }
        if (callbacks.on_extra_key) {
            const MenuContext ctx{state->rows, state->selected, state->filter};
            if (callbacks.on_extra_key(event, ctx, rebuild)) return true;
        }
        if (event.is_character()) {
            state->filter += event.character();
            rebuild();
            return true;
        }
        // Everything below is a key Menu will act on itself (arrow keys, the
        // vim letters are unreachable here -- every character is eaten by the
        // filter above -- but page/home/end/tab are not). Record the
        // direction of the move so a render-pass normalize (below) knows
        // which side to land the cursor on when the move ends on a header.
        if (event == Event::ArrowDown || event == Event::PageDown ||
            event == Event::Tab) {
            state->direction = 1;
        } else if (event == Event::ArrowUp || event == Event::PageUp ||
                   event == Event::TabReverse) {
            state->direction = -1;
        } else if (event == Event::Home) {
            state->direction = 1;   // Home lands on row 0: scan forward from there
        } else if (event == Event::End) {
            state->direction = -1;  // End lands on the last row: scanning backward
        }
        return false;   // arrow keys go to Menu
    });

    // Exposed to the caller (see FilterableMenuSpec::reset_state): the help
    // overlays call it on open so a stale filter never survives a close.
    if (spec.reset_state != nullptr) {
        *spec.reset_state = [state, rebuild] {
            state->filter.clear();
            state->selected = 0;
            state->direction = 1;
            rebuild();
        };
    }

    return Renderer(interactive, [interactive, state, spec, theme] {
        using namespace ftxui;
        // The cursor never rests on a header. Menu's own navigation clamps
        // into bounds but knows nothing of headers (it walks labels), so a
        // move can END on one -- and the transform renders headers with no
        // cursor or highlight, which read as "the selection vanished".
        // Normalizing here (after Menu has applied the move, before the
        // frame is drawn) covers every path that lands on a header: arrows,
        // page/home/end, a mouse-wheel-driven rebuild, and the very first
        // render of a list whose row 0 is a header (the model picker's
        // provider groups). rebuild()'s own call (above) covers the
        // filter-keystroke paths; both read the same last-travel direction.
        if (state->selected >= 0 && state->selected < static_cast<int>(state->rows.size()) &&
            state->rows[state->selected].header) {
            const int resolved =
                ResolveSelectableRow(state->rows, state->selected, state->direction);
            if (resolved >= 0) {
                state->selected = resolved;
                // Menu clamps and takes focus on its next render (its Clamp()
                // runs at the top of OnRender, and SelectedTakeFocus keeps
                // the highlight and vscroll aligned) -- writing its selected
                // ref here is the one sanctioned mutation, the same one
                // on_extra_key handlers already make through rebuild().
            }
        }
        Elements rows;
        // No separate header row here: it used to repeat window_title's own
        // text right below it ("resume a session" / " sessions ") -- the
        // window's title already says what this is.
        //
        // The filter line reads as the input it is -- the typed text with a
        // caret, or a muted prompt when empty -- instead of "filter: (none)".
        // The caller's state (sort order, scope) rides the same line, flush
        // right.
        Elements top;
        if (state->filter.empty()) {
            top.push_back(text(" / type to filter") | Muted(theme));
        } else {
            top.push_back(text(" / " + state->filter));
            top.push_back(text("▏") | Muted(theme));
        }
        top.push_back(filler());
        if (spec.status_suffix) {
            std::string status = spec.status_suffix(state->filter);
            // Suffixes are composed as "  ·  a  ·  b" (appended to the old
            // filter text); standing alone, the leading separator goes.
            const auto start = status.find_first_not_of(" \xC2\xB7");
            status = start == std::string::npos ? std::string() : status.substr(start);
            if (!status.empty()) top.push_back(text(status + " ") | Muted(theme));
        }
        rows.push_back(hbox(std::move(top)));
        rows.push_back(separator() | color(theme.ColorFor("border_unfocused")));
        if (state->rows.size() <= spec.empty_threshold) {
            rows.push_back(text(spec.empty_message) | Muted(theme));
        }
        rows.push_back(interactive->Render() | vscroll_indicator | frame | flex);
        // Key hints in a pinned footer, like the permission/confirm modals --
        // one place to look, whichever overlay is open. paragraph(): a long
        // hint line wraps rather than losing its tail.
        rows.push_back(separator() | color(theme.ColorFor("border_unfocused")));
        rows.push_back(paragraph(spec.hint_line) | Muted(theme));
        // Padded with a space each side, matching prompt_modal.cpp's " permission "/
        // " confirm " -- window() draws the border flush against the title text
        // otherwise. The accent colors the FRAME and title; the rows inside
        // keep the body color (a whole window of accent-colored text had no
        // hierarchy left).
        return window(text(" " + spec.window_title + " ") | bold,
                      vbox(std::move(rows)) | color(theme.ColorFor("assistant_message")) |
                          size(HEIGHT, LESS_THAN, 20)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client
