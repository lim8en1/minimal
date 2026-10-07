#include "filterable_menu.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/utf8.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client::ui_kit {
namespace {

// A row's "#rrggbb"/"#rgb" tint -> Color, tolerant: anything malformed yields
// nullopt (theme default) rather than an exception -- a picker row's cosmetics
// must never break rendering. Downsampled to the theme's depth like every
// theme role: a preset's raw truecolor used to reach 256/16-color terminals.
std::optional<ftxui::Color> RowColor(const std::string& value, ColorDepth depth) {
    const auto rgb = TryParseHexColor(value);
    if (!rgb) return std::nullopt;
    const auto [r, g, b] = *rgb;
    const ftxui::Color c = Downsample(r, g, b, depth);
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
    const std::shared_ptr<MenuMemory> memory = spec.status.memory;
    if (memory) state->filter = memory->filter;

    auto rebuild = [state, build_rows, memory] {
        // Re-anchor by the ROW'S IDENTITY, not its stale index: a filter or
        // sort change can shift what index the previously-highlighted row
        // now lives at (or drop it entirely). Anchoring only the index left
        // the cursor pointing at a DIFFERENT row after such a rebuild --
        // e.g. session_picker.cpp's ctrl+d then deletes the row now sitting
        // at that index, not the one the user actually had highlighted.
        std::optional<std::string> selected_id;
        if (!state->rows.empty() && state->selected >= 0 &&
            state->selected < static_cast<int>(state->rows.size()) &&
            !state->rows[state->selected].header) {
            selected_id = state->rows[state->selected].id;
        } else if (state->rows.empty() && memory && !memory->selected_id.empty()) {
            selected_id = memory->selected_id;   // first build of a rebuilt component
        }
        state->rows = build_rows(state->filter);
        state->labels.clear();
        state->labels.reserve(state->rows.size());
        for (const auto& row : state->rows) state->labels.push_back(row.label);
        const int last = static_cast<int>(state->labels.size()) - 1;
        bool reanchored = false;
        if (selected_id) {
            const auto it = std::find_if(state->rows.begin(), state->rows.end(),
                                          [&](const MenuRow& row) {
                                              return !row.header && row.id == *selected_id;
                                          });
            if (it != state->rows.end()) {
                state->selected = static_cast<int>(it - state->rows.begin());
                reanchored = true;
            }
        }
        if (!reanchored) {
            state->selected = std::clamp(state->selected, 0, std::max(last, 0));
        }
        state->selected =
            ResolveSelectableRow(state->rows, state->selected, state->direction);
        if (memory) memory->filter = state->filter;
    };
    rebuild();

    // Header rows (the model picker's provider groups) get their own entry
    // style: bold, flush-left, never cursor-prefixed or highlighted. Normal rows
    // replicate FTXUI's DefaultOptionTransform plus one indent level, so the
    // two tiers read visually. The transform reads state->rows by index --
    // the same shared block every closure here uses, so it can never outlive
    // or diverge from what Menu itself is displaying.
    MenuOption menu_options;
    // The theme is read per render, never captured by value: /theme swaps
    // it in place, and a captured copy kept a long-lived menu (the help
    // overlays are built once) on the startup palette forever.
    const Theme* theme_ptr = &theme;
    menu_options.entries_option.transform =
        [state, theme_ptr](const EntryState& entry) -> Element {
        const Theme& theme = *theme_ptr;
        const ColorDepth depth = theme.depth();
        const auto selection = SelectionStyle(theme);
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
            // Bounds-checked like the render pass's normalizer below:
            // ResolveSelectableRow returns -1 when the list has no
            // selectable row at all, and `!rows.empty()` alone does not rule
            // that out.
            if (state->selected >= 0 &&
                state->selected < static_cast<int>(state->rows.size()) &&
                !state->rows[state->selected].header && callbacks.on_select) {
                callbacks.on_select(state->rows[state->selected]);
            }
            return true;   // consumed either way: no double-submit on empty list
        }
        // Esc clears a typed filter first, and closes on the next press --
        // one stray Esc no longer throws away a half-typed search along with
        // the menu. ctrl+u clears the filter outright (readline's kill-line).
        if (event == Event::Escape) {
            if (!state->filter.empty()) {
                state->filter.clear();
                rebuild();
            } else if (callbacks.on_close) {
                callbacks.on_close();
            }
            return true;
        }
        if (event == Event::Character(std::string(1, '\x15'))) {
            if (!state->filter.empty()) {
                state->filter.clear();
                rebuild();
            }
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
            // Control bytes (an unbound ctrl+letter) are not filter text.
            const std::string& typed = event.character();
            if (typed.size() == 1 && std::iscntrl(static_cast<unsigned char>(typed[0]))) return true;
            state->filter += typed;
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
        // An empty label list must never reach Menu's own event handling:
        // vendored ftxui's util::clamp tolerates a hi<lo range (this file
        // does not rely on that being true) when there is nothing to select.
        if (state->labels.empty()) return true;
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

    return Renderer(interactive, [interactive, state, spec, theme_ptr, memory] {
        using namespace ftxui;
        const Theme& theme = *theme_ptr;
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
        // Remember the highlight for the next rebuild (MenuMemory): Menu
        // moves `selected` itself on arrow keys, so this is where it lands.
        if (memory && state->selected >= 0 &&
            state->selected < static_cast<int>(state->rows.size()) &&
            !state->rows[state->selected].header) {
            memory->selected_id = state->rows[state->selected].id;
        }
        if (state->rows.size() <= spec.empty_threshold) {
            if (!spec.status.error.empty()) {
                rows.push_back(paragraph(" could not load: " + spec.status.error) |
                               color(theme.ColorFor("error")));
            } else if (spec.status.loading) {
                rows.push_back(text(" loading…") | Muted(theme));
            } else {
                rows.push_back(text(spec.empty_message) | Muted(theme));
            }
        } else if (spec.status.loading) {
            rows.push_back(text(" loading more…") | Muted(theme));
        }
        // Never hand Menu's own Render() an empty label list -- same
        // reasoning as the event guard above; this file keeps the reserved
        // space so the window doesn't jump when a filter starts matching
        // again.
        rows.push_back(state->labels.empty()
                            ? (filler() | flex)
                            : (interactive->Render() | vscroll_indicator | frame | flex));
        // Key hints in a pinned footer, like the permission/confirm modals --
        // one place to look, whichever overlay is open. paragraph(): a long
        // hint line wraps rather than losing its tail.
        rows.push_back(separator() | color(theme.ColorFor("border_unfocused")));
        rows.push_back(paragraph(spec.hint_line) | Muted(theme));
        // Padded with a space each side, matching consent-prompts' consent_modals.cpp's " permission "/
        // " confirm " -- window() draws the border flush against the title text
        // otherwise. The accent colors the FRAME and title; the rows inside
        // keep the body color (a whole window of accent-colored text had no
        // hierarchy left).
        return window(text(" " + spec.window_title + " ") | bold,
                      vbox(std::move(rows)) | color(theme.ColorFor("assistant_message")) |
                          size(HEIGHT, LESS_THAN, kMenuMaxHeight)) |
               color(theme.ColorFor("border_focused"));
    });
}

}  // namespace minimal::client::ui_kit
