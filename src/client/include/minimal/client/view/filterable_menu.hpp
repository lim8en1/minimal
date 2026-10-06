#pragma once

// The shared modal shell behind session_picker.cpp and agent_picker.cpp: a
// type-to-filter Menu over caller-supplied rows, Return/Escape/Backspace
// wiring, and the windowed frame around it. Row building (filter/sort rules,
// which differ genuinely between a paginated session list and a small
// unpaginated preset list) stays in picker_model.cpp/agent_picker_model.cpp
// -- this only owns the FTXUI component wiring both used to duplicate.

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/widget_types.hpp"

namespace minimal::client {

// The index the cursor should occupy, given `from` and the direction the user
// last moved (+1 down, -1 up): `from` itself when it is selectable (in bounds,
// non-header); otherwise the nearest selectable row scanning in `direction`,
// falling back to the opposite side when that side is exhausted, and -1 when
// the list has no selectable row at all (or is empty). Pure so the
// header-skipping rules are directly testable (tests/filterable_menu_test.cpp).
int ResolveSelectableRow(const std::vector<MenuRow>& rows, int from, int direction);

// `build_rows` is called on every filter keystroke and whenever an
// on_extra_key handler invokes the rebuild callback it's given -- it must be
// cheap and side-effect-free (a filter over a captured snapshot, exactly
// what BuildPickerRows/BuildAgentPickerRows already are).
ftxui::Component MakeFilterableMenuModal(
    std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
    const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks, const Theme& theme);

}  // namespace minimal::client
