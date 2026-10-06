#include "minimal/client/view/schema_form_model.hpp"

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
            top.push_back(FieldRow(field, describe, path));
            continue;
        }
        const std::string group = path.substr(0, dot);
        if (!groups.count(group)) group_order.push_back(group);
        groups[group].push_back(FieldRow(field, describe, path.substr(dot + 1)));
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

FormAction ActionFor(const FormRow& row) {
    if (row.type == "boolean") return FormAction::kToggle;
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

}  // namespace minimal::client
