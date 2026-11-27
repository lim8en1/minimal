#include "schema_cache.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <nlohmann/json-schema.hpp>

namespace minimal::node {
namespace {

class CollectingHandler final : public nlohmann::json_schema::basic_error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& /*instance*/,
               const std::string& message) override {
        nlohmann::json_schema::basic_error_handler::error(pointer, nlohmann::json(), message);
        violations.push_back(Violation{pointer.to_string(), message});
    }
    std::vector<Violation> violations;
};

}  // namespace

CompiledSchema::CompiledSchema(const nlohmann::json& schema)
    : validator_(std::make_unique<nlohmann::json_schema::json_validator>(
          nlohmann::json_schema::schema_loader{},
          nlohmann::json_schema::default_string_format_check)) {
    // set_root_schema compiles what it understands and skips the rest, so a
    // typo (`type: strng`) would compile to a schema that accepts anything.
    // Checked against the draft-7 meta-schema first: a contract that is not a
    // schema fails here, not by silently accepting every result.
    // Its `format` keywords (uri-reference, regex) are not checked: the
    // structural errors are the point, and the default checker knows neither.
    static const nlohmann::json_schema::json_validator meta(
        nlohmann::json_schema::draft7_schema_builtin, nlohmann::json_schema::schema_loader{},
        [](const std::string&, const std::string&) {});
    CollectingHandler meta_errors;
    meta.validate(schema, meta_errors);
    if (!meta_errors.violations.empty()) {
        throw std::invalid_argument(FormatViolations(meta_errors.violations, kMaxReportedSchemaErrors));
    }
    // No loader: a $ref to anything outside the schema throws here, at
    // compile time, instead of fetching or failing mid-run.
    try {
        validator_->set_root_schema(schema);
    } catch (const std::exception& error) {
        throw std::invalid_argument(error.what());
    }
}

CompiledSchema::~CompiledSchema() = default;

std::vector<Violation> CompiledSchema::Validate(const nlohmann::json& instance) const {
    CollectingHandler handler;
    validator_->validate(instance, handler);
    return std::move(handler.violations);
}

std::shared_ptr<const CompiledSchema> SchemaCache::Get(const nlohmann::json& schema) {
    std::string key = schema.dump();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = compiled_.find(key); it != compiled_.end()) return it->second;
    }
    // Compiled outside the lock: a slow compile must not serialize every
    // validation behind it. Two racing compiles of one schema both succeed;
    // the first insert wins and the other copy is dropped.
    auto compiled = std::make_shared<const CompiledSchema>(schema);
    std::lock_guard<std::mutex> lock(mutex_);
    return compiled_.emplace(std::move(key), std::move(compiled)).first->second;
}

std::string FormatViolations(const std::vector<Violation>& violations, std::size_t max_lines) {
    std::string out;
    std::size_t shown = 0;
    for (const auto& violation : violations) {
        if (shown == max_lines) {
            out += "... and " + std::to_string(violations.size() - shown) + " more\n";
            break;
        }
        out += (violation.path.empty() ? std::string("(root)") : violation.path) + ": " +
               violation.message + "\n";
        ++shown;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

namespace {

bool StrictObject(const nlohmann::json& schema);

bool StrictNode(const nlohmann::json& schema) {
    if (schema.is_boolean()) return true;
    if (!schema.is_object()) return false;
    const bool is_object_type =
        schema.contains("properties") ||
        (schema.contains("type") &&
         (schema["type"] == "object" ||
          (schema["type"].is_array() &&
           std::find(schema["type"].begin(), schema["type"].end(), "object") !=
               schema["type"].end())));
    if (is_object_type && !StrictObject(schema)) return false;
    if (schema.contains("items") && !StrictNode(schema["items"])) return false;
    for (const char* combinator : {"anyOf", "oneOf", "allOf"}) {
        if (!schema.contains(combinator)) continue;
        if (!schema[combinator].is_array()) return false;
        for (const auto& branch : schema[combinator]) {
            if (!StrictNode(branch)) return false;
        }
    }
    for (const char* defs : {"$defs", "definitions"}) {
        if (!schema.contains(defs)) continue;
        if (!schema[defs].is_object()) return false;
        for (const auto& [name, def] : schema[defs].items()) {
            (void)name;
            if (!StrictNode(def)) return false;
        }
    }
    return true;
}

bool StrictObject(const nlohmann::json& schema) {
    if (!schema.contains("additionalProperties") || schema["additionalProperties"] != false) {
        return false;
    }
    const nlohmann::json properties =
        schema.contains("properties") ? schema["properties"] : nlohmann::json::object();
    if (!properties.is_object()) return false;
    const nlohmann::json required =
        schema.contains("required") ? schema["required"] : nlohmann::json::array();
    if (!required.is_array()) return false;
    for (const auto& [name, property] : properties.items()) {
        if (std::find(required.begin(), required.end(), name) == required.end()) return false;
        if (!StrictNode(property)) return false;
    }
    return true;
}

}  // namespace

bool IsStrictCompatible(const nlohmann::json& schema) { return StrictNode(schema); }

}  // namespace minimal::node
