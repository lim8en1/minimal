#pragma once

// `submit_result`: the node's one way to finish. Its input schema IS the
// preset's output contract (tool_config.node.output_schema, swapped in per
// preset by the definition builder), and a valid call ends the turn with the
// input as the turn's structured result (tools::ToolResult::turn_result).

#include <memory>

#include "minimal/agent/preset_extensions.hpp"
#include "minimal/tools/tool.hpp"

#include "schema_cache.hpp"

namespace minimal::node {

std::unique_ptr<tools::Tool> MakeSubmitResultTool(std::shared_ptr<SchemaCache> schemas);

// The per-preset definition: the output schema as input_schema, strict when
// the schema allows it; an empty name (withheld) for a preset without one.
// Throws std::invalid_argument for a malformed node slice or a schema that
// does not compile, which fails the preset's resolution with that message --
// a node whose contract is broken must not run at all.
llm::ToolDefinition BuildSubmitResultDefinition(const agent::PresetToolContext& context,
                                                SchemaCache& schemas);

}  // namespace minimal::node
