#pragma once

// The pure half of a schema-driven form (schema_form.hpp): rows from a
// config description, and what a key does on a row. No FTXUI, so the rules
// are directly testable. The description is the `plugins` endpoint's
// `describe` answer -- {schema, config, effective, pending} -- whose schema
// is config::SchemaRecording's shape; any other source that produces it
// gets the same form.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace minimal::client {

// One row: a field, or a group header (the first segment of nested paths).
struct FormRow {
    bool header = false;
    std::string path;   // the field's dotted path; "" for a header
    std::string label;  // header: the group; field: the path inside its group
    std::string type;   // the schema type
    std::string shown;  // the value as displayed (a secret's literal masked)
    std::string doc;
    bool live = false;        // "apply": "live"
    bool overridden = false;  // set in the file, not a default
    bool pending = false;     // differs from what the running plugin uses
    // Why the field does nothing right now (its "when" unmet: "strategy is
    // sidecar"); "" when it applies. Shown dimmed, still editable.
    std::string unused;
    nlohmann::json value;     // the effective value
    nlohmann::json field;     // the schema field
};

// Ungrouped (top-level) fields first, then one header per group with its
// fields, each in schema order.
std::vector<FormRow> BuildFormRows(const nlohmann::json& describe);

// What Enter does on a field row: flip a boolean, step an enum to its next
// choice, or open the text editor (everything else).
enum class FormAction { kToggle, kCycle, kEdit };
FormAction ActionFor(const FormRow& row);

// kToggle / kCycle: the text that applies the change ("false", "slow").
std::string NextValueText(const FormRow& row);

// kEdit: the editor's starting text -- a string as is, anything else as JSON.
// A secret starts empty: its literal is never put back on screen.
std::string EditText(const FormRow& row);

// Whether Enter in the editor saves `text`. An empty secret is a cancel, not
// a save: the editor starts a secret empty, so saving "" would erase the
// configured key ('d' is how a secret goes back to its default).
bool SubmitSaves(const FormRow& row, const std::string& text);

// The next selectable (non-header) row from `from` moving by `step`; `from`
// when there is none that way.
int NextFieldRow(const std::vector<FormRow>& rows, int from, int step);

}  // namespace minimal::client
