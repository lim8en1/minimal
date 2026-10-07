#include "minimal/client/sdk/schema_form_model.hpp"

#include <algorithm>
#include <map>

#include "minimal/common/plugin_config_store.hpp"

namespace minimal::client {
namespace {

using nlohmann::json;

FormRow FieldRow(const json& field, const json& describe, const std::string& label) {
    FormRow row;
    row.path = field.value("path", "");
    row.label = label;
    row.type = field.value("type", "json");
    row.doc = field.value("doc", "");
    row.live = field.value("apply", "restart") == "live";
    row.field = field;
    // By reference into `describe`: value() would return a temporary, and the
    // pointer ConfigGet answers with would dangle.
    static const json kNone = json::object();
    const json& config = describe.contains("config") ? describe.at("config") : kNone;
    const json* configured = common::ConfigGet(config, row.path);
    row.overridden = configured != nullptr;
    row.value = configured != nullptr ? *configured : field.value("default", json());
    row.shown = common::DisplayValue(field, row.value);
    if (row.value.is_string() && row.shown == row.value.dump()) {
        row.shown = row.value.get<std::string>();  // plain text, not a JSON literal
        if (row.shown.empty()) row.shown = "\"\"";
    }
    const json pending = describe.value("pending", json::array());
    row.pending = std::find(pending.begin(), pending.end(), row.path) != pending.end();
    if (describe.contains("unused") && describe.at("unused").contains(row.path)) {
        row.unused = describe.at("unused").at(row.path).get<std::string>();
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

// `row`, and for an object list its item rows and add row.
void AppendField(std::vector<FormRow>& rows, FormRow row) {
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
        const auto dot = path.find('.');
        if (dot == std::string::npos) {
            AppendField(top, FieldRow(field, describe, path));
            continue;
        }
        const std::string group = path.substr(0, dot);
        if (!groups.count(group)) group_order.push_back(group);
        AppendField(groups[group], FieldRow(field, describe, path.substr(dot + 1)));
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
    if (row.item >= 0 || row.add_item) return FormAction::kOpen;
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

json ItemDraft(const FormRow& row) {
    const json list = ListOf(row);
    if (row.item >= 0 && row.item < static_cast<int>(list.size())) return list[row.item];
    const json sample = row.field.value("item_template", json::object());
    return sample.is_object() ? sample : json::object();
}

json ItemDescribe(const FormRow& list_row, const json& item) {
    return {{"schema", {{"fields", list_row.field.value("items", json::array())}}},
            {"config", item.is_object() ? item : json::object()}};
}

bool SetItemField(json& item, const FormRow& row, const std::string& text, std::string& error) {
    try {
        common::ConfigSet(item, row.path, common::ParseFieldValue(row.field, text));
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

void UnsetItemField(json& item, const FormRow& row) { common::ConfigUnset(item, row.path); }

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
