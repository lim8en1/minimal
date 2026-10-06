#pragma once

// A form over a config schema, as an FTXUI component: one row per field
// (grouped by the first path segment), each showing its value, whether it is
// set or a default, whether it applies live or on restart, and whether a
// change is pending a restart; the selected field's doc below. Reusable by
// any view that has a `describe`-shaped description (schema_form_model.hpp)
// and somewhere to send edits -- it validates nothing itself: the owner
// sends the text to whoever owns the schema and reports the answer back
// through the state's status line.
//
// Keys: up/down (k/j) select; enter toggles a boolean, steps an enum, or
// opens the inline editor (enter saves, esc cancels; a secret is typed
// masked); d / delete resets the field to its default (a set secret asks for
// a second d); left / backspace goes back (on_back). While `busy` the edit
// keys are ignored; the field's details and the status stay pinned below the
// list.
//
// The component implements EscapeAware: while the editor is open it claims
// Escape, so a plugin modal host cancels the edit instead of closing.

#include <functional>
#include <string>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/widget_types.hpp"

namespace minimal::client {

// `source` is read on every render and event -- it must be cheap (a copy of
// the owner's latest state under its lock).
ftxui::Component MakeSchemaForm(std::function<SchemaFormState()> source,
                                 SchemaFormCallbacks callbacks, const Theme& theme);

}  // namespace minimal::client
