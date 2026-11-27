#pragma once

// The node plugin's slice of an agent preset: `tool_config: {node: {...}}`.
//
//   input_schema   JSON Schema the turn's prompt must satisfy, parsed as JSON
//                  (checked by the input gate before anything runs)
//   output_schema  JSON Schema of the run's result: becomes submit_result's
//                  input_schema, and every submission is validated against it
//   description    optional extra text for submit_result's description
//
// Either schema may be omitted. A preset with no `node` slice is not a node
// and the plugin does nothing for it.

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "schema_cache.hpp"

namespace minimal::node {

inline constexpr const char* kNamespace = "node";
inline constexpr const char* kSubmitToolName = "submit_result";

struct NodeConfig {
    std::optional<nlohmann::json> input_schema;
    std::optional<nlohmann::json> output_schema;
    std::string description;
};

// Parses a `node` slice. Throws std::invalid_argument naming the problem: a
// non-object slice, an unknown key, a schema that is not a JSON object, a
// description that is not a string.
NodeConfig ParseNodeConfig(const nlohmann::json& slice);

// The preset's node slice, or nullptr when it declares none.
const nlohmann::json* FindNodeSlice(const nlohmann::json& tool_config);

}  // namespace minimal::node
