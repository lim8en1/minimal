#pragma once

// Compiled JSON Schemas (draft-7, pboettch/json-schema-validator), shared by
// the submit_result tool, its preset definition builder and the input gate.
// Keyed by the schema's canonical dump, so the same schema in two presets
// compiles once, and an edited preset's new schema simply gets a new entry.

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "policy.hpp"

namespace nlohmann::json_schema {
class json_validator;
}

namespace minimal::node {

// One violation: where in the instance (a JSON pointer, "" = the root) and why.
struct Violation {
    std::string path;
    std::string message;
};

class CompiledSchema {
public:
    explicit CompiledSchema(const nlohmann::json& schema);
    ~CompiledSchema();
    CompiledSchema(const CompiledSchema&) = delete;
    CompiledSchema& operator=(const CompiledSchema&) = delete;

    // Every violation of `instance` (empty = valid). Thread-safe.
    std::vector<Violation> Validate(const nlohmann::json& instance) const;

private:
    std::unique_ptr<nlohmann::json_schema::json_validator> validator_;
};

class SchemaCache {
public:
    // Compiles on first use. Throws std::invalid_argument with the validator's
    // own message for a schema that is not a valid draft-7 schema (or that
    // $refs anything outside itself -- schemas are self-contained).
    std::shared_ptr<const CompiledSchema> Get(const nlohmann::json& schema);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const CompiledSchema>> compiled_;
};

// "path: message" lines, at most `max_lines` of them plus a count of the rest.
std::string FormatViolations(const std::vector<Violation>& violations,
                             std::size_t max_lines = kMaxReportedViolations);

// True when providers' strict tool mode accepts `schema` as-is: every object
// declares additionalProperties: false and requires every property it
// declares, at every level (properties, items, anyOf/oneOf/allOf, $defs,
// definitions). A schema that is not strict-compatible is still enforced --
// by submit_result's own validation -- just not by constrained decoding.
bool IsStrictCompatible(const nlohmann::json& schema);

}  // namespace minimal::node
