#include "minimal/common/plugin_config_store.hpp"

#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <system_error>

#include "minimal/common/atomic_write.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"

namespace minimal::common {
namespace {

using nlohmann::json;

std::vector<std::string> Split(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t dot = path.find('.', start);
        parts.emplace_back(path.substr(start, dot == std::string_view::npos ? path.npos : dot - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return parts;
}

std::string Quote(std::string_view text) { return "\"" + std::string(text) + "\""; }

}  // namespace

const json* ConfigGet(const json& config, std::string_view path) {
    const json* node = &config;
    for (const auto& part : Split(path)) {
        if (!node->is_object()) return nullptr;
        const auto it = node->find(part);
        if (it == node->end()) return nullptr;
        node = &*it;
    }
    return node;
}

void ConfigSet(json& config, std::string_view path, json value) {
    const auto parts = Split(path);
    json* node = &config;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        json& next = (*node)[parts[i]];
        if (next.is_null()) next = json::object();
        if (!next.is_object()) {
            throw std::invalid_argument(Quote(parts[i]) + " holds a value, not an object");
        }
        node = &next;
    }
    (*node)[parts.back()] = std::move(value);
}

bool ConfigUnset(json& config, std::string_view path) {
    const auto parts = Split(path);
    // The chain of objects down to the leaf's parent, to prune on the way up.
    std::vector<json*> chain{&config};
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        json* node = chain.back();
        if (!node->is_object() || !node->contains(parts[i])) return false;
        chain.push_back(&(*node)[parts[i]]);
    }
    if (!chain.back()->is_object() || chain.back()->erase(parts.back()) == 0) return false;
    for (std::size_t i = chain.size() - 1; i > 0; --i) {
        if (!chain[i]->empty()) break;
        chain[i - 1]->erase(parts[i - 1]);
    }
    return true;
}

const json* SchemaField(const json& schema, std::string_view path) {
    if (!schema.is_object() || !schema.contains("fields")) return nullptr;
    for (const auto& field : schema.at("fields")) {
        if (field.value("path", "") == path) return &field;
    }
    return nullptr;
}

json ParseFieldValue(const json& field, std::string_view text) {
    const std::string type = field.value("type", "json");
    const std::string path = field.value("path", "");
    const auto fail = [&](const std::string& message) -> json {
        throw std::invalid_argument(path + ": " + message);
    };
    if (type == "string") return std::string(text);
    if (type == "enum") {
        std::string list;
        for (const auto& choice : field.value("choices", json::array())) {
            if (choice == text) return std::string(text);
            list += (list.empty() ? "" : ", ") + choice.dump();
        }
        return fail("must be one of " + list + ", got " + Quote(text));
    }
    if (type == "boolean") {
        if (text == "true") return true;
        if (text == "false") return false;
        return fail("must be true or false, got " + Quote(text));
    }
    if (type == "integer") {
        std::size_t used = 0;
        long long value = 0;
        try {
            value = std::stoll(std::string(text), &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used == 0 || used != text.size()) return fail("must be an integer, got " + Quote(text));
        if (field.contains("minimum") && value < field.at("minimum").get<long long>()) {
            return fail("must be at least " + field.at("minimum").dump() + ", got " + Quote(text));
        }
        if (field.contains("maximum") && value > field.at("maximum").get<long long>()) {
            return fail("must be at most " + field.at("maximum").dump() + ", got " + Quote(text));
        }
        return value;
    }
    if (type == "number") {
        std::size_t used = 0;
        double value = 0;
        try {
            value = std::stod(std::string(text), &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used == 0 || used != text.size() || !std::isfinite(value)) {
            return fail("must be a number, got " + Quote(text));
        }
        if ((field.contains("minimum") && value < field.at("minimum").get<double>()) ||
            (field.contains("maximum") && value > field.at("maximum").get<double>())) {
            return fail("must be between " + field.value("minimum", json()).dump() + " and " +
                        field.value("maximum", json()).dump() + ", got " + Quote(text));
        }
        return value;
    }
    // string_list, string_map, json: the text is JSON.
    const json parsed = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) return fail("must be JSON, got " + Quote(text));
    if (type == "string_list" && !parsed.is_array()) return fail("must be a JSON list of strings");
    if (type == "string_map" && !parsed.is_object()) return fail("must be a JSON object");
    return parsed;
}

std::optional<std::string> UnusedReason(const json& schema, const json& field,
                                        const json& config) {
    if (!field.is_object() || !field.contains("when") || !field.at("when").is_object()) {
        return std::nullopt;
    }
    const json& when = field.at("when");
    const std::string on = when.value("field", "");
    const json* condition_field = SchemaField(schema, on);
    const json* configured = ConfigGet(config, on);
    const json value = configured != nullptr ? *configured
                       : condition_field != nullptr ? condition_field->value("default", json())
                                                    : json();
    if (when.value("set", false)) {
        const bool set = !(value.is_null() || (value.is_string() && value.get<std::string>().empty()));
        if (set) return std::nullopt;
        return on + " is not set";
    }
    for (const auto& allowed : when.value("in", json::array())) {
        if (allowed == value) return std::nullopt;
    }
    return on + " is " + (value.is_string() ? value.get<std::string>() : value.dump());
}

std::vector<EffectiveField> EffectiveConfig(const json& schema, const json& config) {
    std::vector<EffectiveField> out;
    if (!schema.is_object() || !schema.contains("fields")) return out;
    for (const auto& field : schema.at("fields")) {
        EffectiveField row;
        row.path = field.value("path", "");
        row.field = &field;
        if (const json* value = ConfigGet(config, row.path)) {
            row.value = *value;
            row.overridden = true;
        } else {
            row.value = field.value("default", json());
        }
        row.unused = UnusedReason(schema, field, config);
        out.push_back(std::move(row));
    }
    return out;
}

ConfigChange DiffConfig(const json& schema, const json& before, const json& after) {
    ConfigChange change;
    const auto old_rows = EffectiveConfig(schema, before);
    const auto new_rows = EffectiveConfig(schema, after);
    for (std::size_t i = 0; i < old_rows.size(); ++i) {
        if (old_rows[i].value == new_rows[i].value) continue;
        if (old_rows[i].unused && new_rows[i].unused) {
            change.unused.push_back(old_rows[i].path);
            continue;
        }
        change.paths.push_back(old_rows[i].path);
        if (old_rows[i].field->value("apply", "restart") != "live") change.all_live = false;
    }
    // A key the schema does not list (only possible in a hand-edited file):
    // the plugin will refuse it anyway, and nothing about it is live.
    // flatten() of {} is one entry under the empty pointer, not none.
    const auto flat = [](const json& config) {
        return config.is_object() && !config.empty() ? config.flatten() : json::object();
    };
    const json flat_before = flat(before);
    const json flat_after = flat(after);
    std::set<std::string> pointers;
    for (const auto& [pointer, value] : flat_before.items()) pointers.insert(pointer);
    for (const auto& [pointer, value] : flat_after.items()) pointers.insert(pointer);
    for (const auto& pointer : pointers) {
        if (pointer.empty()) continue;
        std::string path = pointer.substr(1);
        for (auto& c : path) c = c == '/' ? '.' : c;
        // Inside a schema field (a "json" field's subtree): compared above.
        bool known = false;
        for (std::string prefix = path;; prefix = prefix.substr(0, prefix.rfind('.'))) {
            if (SchemaField(schema, prefix) != nullptr) known = true;
            if (known || prefix.find('.') == std::string::npos) break;
        }
        if (known) continue;
        if (flat_before.value(pointer, json()) == flat_after.value(pointer, json())) continue;
        change.paths.push_back(path);
        change.all_live = false;
    }
    return change;
}

std::string DisplayValue(const json& field, const json& value) {
    if (field.value("secret", false) && value.is_string()) {
        const std::string text = value.get<std::string>();
        if (text.empty() || text.rfind("secret:", 0) == 0 || text.rfind("env:", 0) == 0) {
            return value.dump();
        }
        return "\"****\"";
    }
    return value.is_null() ? "(unset)" : value.dump();
}

json ReadPluginConfigFile(const std::filesystem::path& path, const std::string& label) {
    json loaded = config::LoadJsonConfig<json>(
        path, label, config::AbsencePolicy::kDefaultOnMissing,
        config::EmptyFilePolicy::kTreatAsAbsent, [&](const json& doc) {
            if (!doc.is_object()) throw config::ConfigError(label + ": must be a JSON object");
            return doc;
        });
    return loaded.is_object() ? loaded : json::object();
}

std::string WritePluginConfigFile(const std::filesystem::path& path, const json& config) {
    std::error_code ec;
    if (config.empty()) {
        std::filesystem::remove(path, ec);
        return ec ? "could not remove " + path.string() + ": " + ec.message() : "";
    }
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return "could not create " + path.parent_path().string() + ": " + ec.message();
    const WriteOutcome outcome = write_file(path, config.dump(2) + "\n");
    return outcome.ok ? "" : "could not write " + path.string() + ": " + outcome.error;
}

}  // namespace minimal::common
