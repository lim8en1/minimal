#pragma once

// The shared modal shell behind session_picker.cpp and agent_picker.cpp: a
// type-to-filter Menu over caller-supplied rows, Return/Escape/Backspace
// wiring, and the windowed frame around it. Row building (filter/sort rules,
// which differ genuinely between a paginated session list and a small
// unpaginated preset list) stays in picker_model.cpp/agent_picker_model.cpp
// -- this only owns the FTXUI component wiring both used to duplicate.

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/render/theme.hpp"

namespace minimal::client {

// One row of the filtered list. `id` is caller-defined: session_id for the
// session picker (empty => the synthetic "+ new session" row), preset name
// for the agent picker, "<provider>/<model>" for the model picker.
struct MenuRow {
    std::string label;
    std::string id;
    // A header row is a group label (the model picker's provider names):
    // rendered bold and never prefixed with the cursor, and Enter on it is
    // swallowed. It IS highlightable -- Menu owns arrow-key navigation, and
    // hand-rolling a skipping list for one picker's sake was not worth it.
    bool header = false;
    // Optional "#rrggbb" for the label (the agent picker's preset.color).
    // Anything unparseable renders as the theme default -- a picker row must
    // never throw over cosmetics.
    std::string color;
};

// What on_extra_key needs to answer a key the generic handler doesn't
// recognize: the session picker's ctrl+d (delete the highlighted row) and
// arrow-down-at-end (load more, only when unfiltered and already on the
// last row) both need the CURRENT row list, the highlighted index, and the
// filter text, not just the event itself.
struct MenuContext {
    const std::vector<MenuRow>& rows;
    int selected;
    const std::string& filter;
};

struct FilterableMenuCallbacks {
    // Return, with the highlighted row (rows is never empty when this
    // fires -- Return is swallowed unconditionally on an empty list, same
    // as the two pickers already did, so there is nothing to select).
    std::function<void(const MenuRow&)> on_select;
    std::function<void()> on_close;   // Escape

    // Keys the generic handler doesn't recognize (session picker: ctrl+d,
    // tab, ctrl+a, arrow-down-at-end; agent picker: nullptr, it has none).
    // `rebuild` is the shell's OWN rebuild -- call it if the handler
    // mutated something that changes what build_rows(filter) would return
    // (e.g. the session picker's Tab cycling its own external sort state).
    // Returns whether the event was consumed.
    std::function<bool(const ftxui::Event&, const MenuContext&, const std::function<void()>& rebuild)>
        on_extra_key;
};

struct FilterableMenuSpec {
    std::string window_title;    // "resume a session" / "switch agent preset"
    std::string hint_line;       // caller-composed: key hints differ per picker
    // Appended after " filter: X" (or "(none)") -- the session picker adds
    // sort/all-dirs/more-below; the agent picker leaves this empty. Composed
    // fresh each render so it reflects state (sort order, has_more) the
    // shell has no access to.
    std::function<std::string(const std::string& filter)> status_suffix;
    std::string empty_message;   // "(no other stored sessions here)" / "(no agent presets match)"
    // The empty message shows when build_rows(filter).size() <= this. The
    // session picker always has its synthetic "+ new session" row, so it
    // sets this to 1; the agent picker (no synthetic row) leaves it 0.
    std::size_t empty_threshold = 0;
    // If set, the factory stores a closure clearing the filter and the
    // cursor selection. The help overlays are built ONCE (app.cpp), so
    // without this the typed filter survived across opens -- reopening the
    // help presented yesterday's needle as if it were a fresh search. The
    // PICKERS deliberately do not reset (reopening with the filter intact
    // is the feature there).
    std::function<void()>* reset_state = nullptr;
};

// `build_rows` is called on every filter keystroke and whenever an
// on_extra_key handler invokes the rebuild callback it's given -- it must be
// cheap and side-effect-free (a filter over a captured snapshot, exactly
// what BuildPickerRows/BuildAgentPickerRows already are).
ftxui::Component MakeFilterableMenuModal(
    std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
    const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks, const Theme& theme);

}  // namespace minimal::client
