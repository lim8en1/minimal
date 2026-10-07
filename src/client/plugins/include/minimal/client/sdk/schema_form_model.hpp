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
    // An object_list field is followed by one row per item and an "add"
    // row. Those carry the LIST's path, field and value; `item` is the
    // item's index (-1 on every other row), `add_item` marks the add row.
    int item = -1;
    bool add_item = false;
    // An object_map's entry row: the entry's name ("" on every other row).
    std::string item_key;
};

// The synthetic field a map entry's form shows its name under (an entry's
// draft carries its name there; MapWithEntry takes it back out).
inline constexpr const char* kMapKeyField = "@key";

// Ungrouped (top-level) fields first, then one header per group with its
// fields, each in schema order.
std::vector<FormRow> BuildFormRows(const nlohmann::json& describe);

// What Enter does on a field row: flip a boolean, step an enum to its next
// choice, open the text editor (everything else) -- or, when the form's owner
// can show a chooser (`can_pick`), hand it an enum or a field whose
// "format" names what it holds (a model ref): stepping an enum wrote every
// choice it passed through, and a typed model ref was a guess.
// kOpen: an object list's item (or its add row) opens as a form of its own.
enum class FormAction { kToggle, kCycle, kEdit, kPick, kOpen };
FormAction ActionFor(const FormRow& row, bool can_pick = false);

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

// --- Object lists ---------------------------------------------------------------
//
// An item is edited as a form of its own, over a draft the owner keeps; the
// whole list is sent once the item is done (the plugin validates the list,
// so an item is checked in context: a duplicate name, a missing field).

// The list an item row (or the list row, or its add row) belongs to.
nlohmann::json ListOf(const FormRow& row);
// A schema-less value (an object or array) as a describe: one "member"
// field per key or index, typed by its value; a nested object or array is a
// "json_node" row that opens as a form of its own.
nlohmann::json InferredDescribe(const nlohmann::json& value);
// A map entry's draft back into `map` under its (possibly new) name. False,
// with `error`, for an empty name or one another entry already has.
bool MapWithEntry(nlohmann::json& map, const std::string& old_key, nlohmann::json draft,
                  std::string& error);
nlohmann::json MapWithout(const nlohmann::json& map, const std::string& key);
// `value` set into `parent` at `row`: a member by its key or index, a
// described field by its path.
void SetNode(nlohmann::json& parent, const FormRow& row, nlohmann::json value);
// A typed value for a schema-less member: JSON when it parses ("3", "true",
// "[1]"), the text itself otherwise.
nlohmann::json ParseLooseValue(const std::string& text);
// The draft an item row opens with; the add row's is the schema's template.
nlohmann::json ItemDraft(const FormRow& row);
// The describe an item's form is built from (BuildFormRows): the item
// fields as the schema, `item` as the config.
nlohmann::json ItemDescribe(const FormRow& list_row, const nlohmann::json& item);
// An edit on an item form's row, applied to the draft. False, with `error`,
// when `text` does not parse as the field's type (the draft is unchanged).
bool SetItemField(nlohmann::json& item, const FormRow& row, const std::string& text,
                  std::string& error);
void UnsetItemField(nlohmann::json& item, const FormRow& row);
// The list with `item` at `index` (-1: appended), without `index`, or with
// `index` moved by `step` (clamped to the list).
nlohmann::json ListWithItem(const nlohmann::json& list, int index, const nlohmann::json& item);
nlohmann::json ListWithout(const nlohmann::json& list, int index);
nlohmann::json ListMoved(const nlohmann::json& list, int index, int step);

}  // namespace minimal::client
