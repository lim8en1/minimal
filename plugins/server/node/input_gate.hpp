#pragma once

// The node's input contract: a turn gate that rejects a prompt which is not
// JSON, or does not satisfy the preset's tool_config.node.input_schema,
// before anything runs (StopReason::Rejected; headless exit 65).

#include <memory>

#include "minimal/agent/turn_gate.hpp"

#include "schema_cache.hpp"

namespace minimal::node {

std::shared_ptr<agent::TurnGate> MakeInputGate(std::shared_ptr<SchemaCache> schemas);

}  // namespace minimal::node
