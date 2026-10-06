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
// opens the inline editor (enter saves, esc cancels); d / delete resets the
// field to its default; left / backspace goes back (on_back).
//
// The component implements EscapeAware: while the editor is open it claims
// Escape, so a plugin modal host cancels the edit instead of closing.

#include <cstdint>
#include <functional>
#include <string>

#include <ftxui/component/component.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/render/theme.hpp"

namespace minimal::client {

struct SchemaFormState {
    std::string title;
    nlohmann::json describe;  // null while loading
    std::string status;       // the last edit's outcome, or a load error
    bool status_is_error = false;
    // Bumped whenever `describe` changes: the rows rebuild, keeping the
    // selected field.
    std::uint64_t generation = 0;
};

struct SchemaFormCallbacks {
    std::function<void(const std::string& path, const std::string& text)> on_set;
    std::function<void(const std::string& path)> on_unset;
    std::function<void()> on_back;  // optional
};

// `source` is read on every render and event -- it must be cheap (a copy of
// the owner's latest state under its lock).
ftxui::Component MakeSchemaForm(std::function<SchemaFormState()> source,
                                 SchemaFormCallbacks callbacks, const Theme& theme);

}  // namespace minimal::client
