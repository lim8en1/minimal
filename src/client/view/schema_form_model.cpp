#include "minimal/client/sdk/schema_form_model.hpp"

#include <algorithm>
#include <map>

#include "minimal/common/plugin_config_store.hpp"

namespace minimal::client {
namespace {

using nlohmann::json;

// A "member" field names one key (or array index) of its config as it is,
// dots included -- a model id "gpt-4.1" is not a group -- rather than a
// dotted path into it.
bool IsMember(const json& field) { return field.value("member", false); }

const json* MemberOf(const json& config, const std::string& key) {
    if (config.is_object()) {
        const auto it = config.find(key);
        return it != config.end() ? &*it : nullptr;
    }
    if (config.is_array()) {
        try {
            const std::size_t index = std::stoul(key);
            return index < config.size() ? &config[index] : nullptr;
        } catch (const std::exception&) {
            return nullptr;
        }
    }
    return nullptr;
}

// "{…} 3 keys" / "[…] 2 items": a nested value's row.
std::string NodeSummary(const json& value) {
    if (value.is_object()) return "{…} " + std::to_string(value.size()) + (value.size() == 1 ? " key" : " keys");
    if (value.is_array()) return "[…] " + std::to_string(value.size()) + (value.size() == 1 ? " item" : " items");
    return value.dump();
}

// The describe's check findings, or null.
json ResultsOf(const json& describe) {
    if (!describe.is_object() || !describe.contains("checks") || !describe.at("checks").is_object()) {
        return nullptr;
    }
    return describe.at("checks").value("results", json());
}

int Severity(const std::string& level) {
    return level == "error" ? 3 : level == "warning" ? 2 : level == "ok" ? 1 : 0;  // NOLINT(readability-magic-numbers): an order, not quantities
}

// `result` on `row`, when it is worse than what the row already has.
void Worst(FormRow& row, const json& result) {
    const std::string level = result.value("level", "");
    if (Severity(level) <= Severity(row.check_level)) return;
    row.check_level = level;
    row.check_message = result.value("message", "");
}

// An entry row's check: the worst finding under its prefix.
void EntryCheck(FormRow& row, const json& describe, const std::string& prefix) {
    const json results = ResultsOf(describe);
    if (!results.is_array()) return;
    for (const auto& result : results) {
        const std::string path = result.value("path", "");
        if (path.rfind(prefix, 0) == 0 || path == prefix.substr(0, prefix.size() - 1)) Worst(row, result);
    }
}

FormRow FieldRow(const json& field, const json& describe, const std::string& label) {
    FormRow row;
    row.path = field.value("path", "");
    row.label = field.value("label", label);
    row.type = field.value("type", "json");
    row.doc = field.value("doc", "");
    row.live = field.value("apply", "restart") == "live";
    row.field = field;
    // By reference into `describe`: value() would return a temporary, and the
    // pointer ConfigGet answers with would dangle.
    static const json kNone = json::object();
    const json& config = describe.contains("config") ? describe.at("config") : kNone;
    const json* configured =
        IsMember(field) ? MemberOf(config, row.path) : common::ConfigGet(config, row.path);
    row.overridden = configured != nullptr;
    row.value = configured != nullptr ? *configured : field.value("default", json());
    // Structure reads as its shape ("{…} 3 keys"): its members are a form away.
    const bool structured = row.type == "json_node" ||
                            (row.type == "json" && (row.value.is_object() || row.value.is_array()));
    row.shown = structured ? NodeSummary(row.value) : common::DisplayValue(field, row.value);
    row.holds_secret = common::HoldsMaskedSecret(field, row.value);
    if (row.value.is_string() && row.shown == row.value.dump()) {
        row.shown = row.value.get<std::string>();  // plain text, not a JSON literal
        if (row.shown.empty()) row.shown = "\"\"";
    }
    const json pending = describe.value("pending", json::array());
    row.pending = std::find(pending.begin(), pending.end(), row.path) != pending.end();
    if (describe.contains("unused") && describe.at("unused").contains(row.path)) {
        row.unused = describe.at("unused").at(row.path).get<std::string>();
    }
    if (const json results = ResultsOf(describe); results.is_array()) {
        for (const auto& result : results) {
            if (result.value("path", "") != row.path) continue;
            Worst(row, result);
        }
    }
    return row;
}

// One item's line: its set values in schema order, secrets masked.
std::string ItemSummary(const json& items, const json& item) {
    std::string out;
    for (const auto& field : items) {
        const std::string key = field.value("path", "");
        const json value = item.is_object() && item.contains(key) ? item.at(key)
                                                                  : field.value("default", json());
        if (value.is_null() || (value.is_string() && value.get<std::string>().empty())) continue;
        std::string shown = common::DisplayValue(field, value);
        if (value.is_string() && shown == value.dump()) shown = value.get<std::string>();
        out += (out.empty() ? "" : "  ") + shown;
    }
    return out.empty() ? "(empty)" : out;
}

// `row`, and for an object map its entry rows (by name) and add row.
void AppendMap(std::vector<FormRow>& rows, FormRow row, const json& describe) {
    const json map = row.value.is_object() ? row.value : json::object();
    const json items = row.field.value("items", json::array());
    row.shown = map.empty() ? "(none)" : std::to_string(map.size()) + (map.size() == 1 ? " entry" : " entries");
    const FormRow base = row;
    rows.push_back(std::move(row));
    for (const auto& [name, entry] : map.items()) {
        if (name.rfind("//", 0) == 0) continue;   // a comment key
        FormRow item = base;
        item.item_key = name;
        item.label = name;
        item.shown = ItemSummary(items, entry);
        item.check_level.clear();
        item.check_message.clear();
        EntryCheck(item, describe, base.path + "." + name + ".");
        rows.push_back(std::move(item));
    }
    FormRow add = base;
    add.add_item = true;
    add.label = "+";
    add.shown = "add an entry";
    add.overridden = false;
    rows.push_back(std::move(add));
}

// `row`, and for an object list its item rows and add row.
void AppendField(std::vector<FormRow>& rows, FormRow row, const json& describe) {
    if (row.type == "object_map") {
        AppendMap(rows, std::move(row), describe);
        return;
    }
    if (row.type != "object_list") {
        rows.push_back(std::move(row));
        return;
    }
    const json list = row.value.is_array() ? row.value : json::array();
    const json items = row.field.value("items", json::array());
    row.shown = list.empty() ? "(none)" : std::to_string(list.size()) + (list.size() == 1 ? " entry" : " entries");
    const FormRow base = row;
    rows.push_back(std::move(row));
    for (std::size_t i = 0; i < list.size(); ++i) {
        FormRow item = base;
        item.item = static_cast<int>(i);
        item.label = std::to_string(i + 1);
        item.shown = ItemSummary(items, list[i]);
        item.check_level.clear();
        item.check_message.clear();
        EntryCheck(item, describe, base.path + "[" + std::to_string(i) + "].");
        rows.push_back(std::move(item));
    }
    FormRow add = base;
    add.add_item = true;
    add.label = "+";
    add.shown = "add an entry";
    add.overridden = false;
    rows.push_back(std::move(add));
}

}  // namespace

std::vector<FormRow> BuildFormRows(const json& describe) {
    std::vector<FormRow> top;
    std::vector<std::string> group_order;
    std::map<std::string, std::vector<FormRow>> groups;
    if (!describe.is_object() || !describe.contains("schema")) return top;
    for (const auto& field : describe.at("schema").value("fields", json::array())) {
        const std::string path = field.value("path", "");
        const auto dot = IsMember(field) ? std::string::npos : path.find('.');
        if (dot == std::string::npos) {
            AppendField(top, FieldRow(field, describe, path), describe);
            continue;
        }
        const std::string group = path.substr(0, dot);
        if (!groups.count(group)) group_order.push_back(group);
        AppendField(groups[group], FieldRow(field, describe, path.substr(dot + 1)), describe);
    }
    std::vector<FormRow> rows = std::move(top);
    for (const auto& group : group_order) {
        FormRow header;
        header.header = true;
        header.label = group;
        rows.push_back(std::move(header));
        for (auto& row : groups[group]) rows.push_back(std::move(row));
    }
    return rows;
}

FormAction ActionFor(const FormRow& row, bool can_pick) {
    if (row.item >= 0 || !row.item_key.empty() || row.add_item) return FormAction::kOpen;
    if (row.type == "json_node") return FormAction::kOpen;
    // A schema-less JSON value with structure opens as an inferred form.
    if (row.type == "json" && (row.value.is_object() || row.value.is_array())) return FormAction::kOpen;
    if (row.type == "boolean") return FormAction::kToggle;
    if (can_pick && (row.type == "enum" || row.field.contains("format"))) return FormAction::kPick;
    if (row.type == "enum") return FormAction::kCycle;
    return FormAction::kEdit;
}

std::string NextValueText(const FormRow& row) {
    if (row.type == "boolean") return row.value == true ? "false" : "true";
    const json choices = row.field.value("choices", json::array());
    if (choices.empty()) return row.value.is_string() ? row.value.get<std::string>() : "";
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i] == row.value) return choices[(i + 1) % choices.size()].get<std::string>();
    }
    return choices[0].get<std::string>();
}

std::string EditText(const FormRow& row) {
    if (row.field.value("secret", false)) return {};
    if (row.value.is_string()) return row.value.get<std::string>();
    if (row.value.is_null()) return {};
    return row.value.dump();
}

bool SubmitSaves(const FormRow& row, const std::string& text) {
    return !(row.field.value("secret", false) && text.empty());
}

int NextFieldRow(const std::vector<FormRow>& rows, int from, int step) {
    for (int i = from + step; i >= 0 && i < static_cast<int>(rows.size()); i += step) {
        if (!rows[i].header) return i;
    }
    return from;
}

json ListOf(const FormRow& row) { return row.value.is_array() ? row.value : json::array(); }

json ChecksUnder(const json& checks, const std::string& prefix) {
    if (!checks.is_object()) return nullptr;
    json out = checks;
    out["results"] = json::array();
    for (const auto& result : checks.value("results", json::array())) {
        const std::string path = result.value("path", "");
        if (path.rfind(prefix, 0) != 0) continue;
        json relative = result;
        relative["path"] = path.substr(prefix.size());
        out["results"].push_back(std::move(relative));
    }
    return out;
}

std::string CheckSummary(const json& describe) {
    if (!describe.is_object() || !describe.contains("checks") || !describe.at("checks").is_object()) {
        return {};
    }
    const json& checks = describe.at("checks");
    if (checks.value("state", "") == "running") return "checking...";
    if (checks.value("state", "") != "done") return {};
    int errors = 0;
    int warnings = 0;
    for (const auto& result : checks.value("results", json::array())) {
        const std::string level = result.value("level", "");
        errors += level == "error" ? 1 : 0;
        warnings += level == "warning" ? 1 : 0;
    }
    if (errors == 0 && warnings == 0) return "checks: all good";
    std::string out = "checks:";
    if (errors > 0) out += " " + std::to_string(errors) + (errors == 1 ? " error" : " errors");
    if (warnings > 0) {
        out += std::string(errors > 0 ? "," : "") + " " + std::to_string(warnings) +
               (warnings == 1 ? " warning" : " warnings");
    }
    return out;
}

json ItemDraft(const FormRow& row) {
    if (row.type == "object_map") {
        const json map = row.value.is_object() ? row.value : json::object();
        json draft = !row.item_key.empty() && map.contains(row.item_key)
                         ? map.at(row.item_key)
                         : row.field.value("item_template", json::object());
        if (!draft.is_object()) draft = json::object();
        draft[std::string(kMapKeyField)] = row.item_key;
        return draft;
    }
    const json list = ListOf(row);
    if (row.item >= 0 && row.item < static_cast<int>(list.size())) return list[row.item];
    const json sample = row.field.value("item_template", json::object());
    return sample.is_object() ? sample : json::object();
}

json ItemDescribe(const FormRow& list_row, const json& item) {
    json fields = list_row.field.value("items", json::array());
    if (list_row.type == "object_map") {
        // The entry's name, first: what the key describes (its doc, format --
        // a model id gets the model chooser), required.
        json key = list_row.field.value("key", json::object());
        key["path"] = kMapKeyField;
        key["label"] = "name";
        key["type"] = "string";
        key["required"] = true;
        key["member"] = true;
        fields.insert(fields.begin(), std::move(key));
    }
    return {{"schema", {{"fields", std::move(fields)}}},
            {"config", item.is_object() ? item : json::object()}};
}

json InferredDescribe(const json& value) {
    json fields = json::array();
    const auto field_for = [](const std::string& path, const json& member, std::string label) {
        std::string type = "string";
        if (member.is_boolean()) type = "boolean";
        else if (member.is_number_integer()) type = "integer";
        else if (member.is_number()) type = "number";
        else if (member.is_object() || member.is_array()) type = "json_node";
        else if (member.is_null()) type = "json";
        return json{{"path", path}, {"label", std::move(label)}, {"type", type}, {"member", true}};
    };
    if (value.is_object()) {
        for (const auto& [key, member] : value.items()) fields.push_back(field_for(key, member, key));
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) {
            fields.push_back(field_for(std::to_string(i), value[i], "[" + std::to_string(i) + "]"));
        }
    }
    return {{"schema", {{"fields", std::move(fields)}}}, {"config", value}};
}

json* MemberSlot(json& draft, const FormRow& row) {
    if (draft.is_array()) {
        try {
            const std::size_t index = std::stoul(row.path);
            return index < draft.size() ? &draft[index] : nullptr;
        } catch (const std::exception&) {
            return nullptr;
        }
    }
    if (!draft.is_object()) draft = json::object();
    return &draft[row.path];
}

void SetNode(json& parent, const FormRow& row, json value) {
    if (!IsMember(row.field)) {
        common::ConfigSet(parent, row.path, std::move(value));
        return;
    }
    if (json* slot = MemberSlot(parent, row)) *slot = std::move(value);
}

bool SetItemField(json& item, const FormRow& row, const std::string& text, std::string& error) {
    try {
        json value = common::ParseFieldValue(row.field, text);
        if (IsMember(row.field)) {
            json* slot = MemberSlot(item, row);
            if (slot == nullptr) throw std::invalid_argument(row.path + ": no such item");
            *slot = std::move(value);
        } else {
            common::ConfigSet(item, row.path, std::move(value));
        }
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

void UnsetItemField(json& item, const FormRow& row) {
    if (!IsMember(row.field)) {
        common::ConfigUnset(item, row.path);
        return;
    }
    if (item.is_object()) {
        item.erase(row.path);
    } else if (item.is_array()) {
        try {
            const std::size_t index = std::stoul(row.path);
            if (index < item.size()) item.erase(item.begin() + static_cast<long>(index));
        } catch (const std::exception&) {
        }
    }
}

bool MapWithEntry(json& map, const std::string& old_key, json draft, std::string& error) {
    if (!map.is_object()) map = json::object();
    const std::string key = draft.value(std::string(kMapKeyField), std::string());
    draft.erase(std::string(kMapKeyField));
    if (key.empty()) {
        error = "give the entry a name";
        return false;
    }
    if (key != old_key && map.contains(key)) {
        error = "\"" + key + "\" already has an entry";
        return false;
    }
    if (!old_key.empty() && key != old_key) map.erase(old_key);
    map[key] = std::move(draft);
    return true;
}

json MapWithout(const json& map, const std::string& key) {
    json out = map.is_object() ? map : json::object();
    out.erase(key);
    return out;
}

json ParseLooseValue(const std::string& text) {
    json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    return parsed.is_discarded() ? json(text) : parsed;
}

json ListWithItem(const json& list, int index, const json& item) {
    json out = list.is_array() ? list : json::array();
    if (index >= 0 && index < static_cast<int>(out.size())) {
        out[index] = item;
    } else {
        out.push_back(item);
    }
    return out;
}

json ListWithout(const json& list, int index) {
    json out = list.is_array() ? list : json::array();
    if (index >= 0 && index < static_cast<int>(out.size())) out.erase(out.begin() + index);
    return out;
}

json ListMoved(const json& list, int index, int step) {
    json out = list.is_array() ? list : json::array();
    const int size = static_cast<int>(out.size());
    if (index < 0 || index >= size) return out;
    const int to = std::clamp(index + step, 0, size - 1);
    if (to == index) return out;
    json moved = out[index];
    out.erase(out.begin() + index);
    out.insert(out.begin() + to, std::move(moved));
    return out;
}

}  // namespace minimal::client
