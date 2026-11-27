// node -- an agent preset as a pipeline step with JSON contracts.
//
// A preset becomes a node by declaring `tool_config: {node: {input_schema,
// output_schema}}` (node_config.hpp). The plugin then owns both ends:
//
//   input   a turn gate parses the prompt as JSON and validates it against
//           input_schema before anything runs; a violation rejects the turn
//           (StopReason::Rejected -- `minimal -p -` exits 65).
//   output  the `submit_result` tool advertises output_schema as its input
//           schema (a preset definition builder), validates every call
//           against it, and a valid call finishes the turn with the input as
//           the turn's structured result (ToolResult::turn_result --
//           `minimal --output-format result` prints it alone).
//
// Pair it with `tool_choice: required` in the preset so the model cannot end
// the turn in prose: the only way out is a valid submit_result (or the turn
// ceiling). Core knows nothing about schemas; every contract decision is here.

#include "minimal/plugins/plugin_abi.h"

#include <memory>

#include "input_gate.hpp"
#include "node_config.hpp"
#include "schema_cache.hpp"
#include "submit_result_tool.hpp"

namespace {

// One cache for the tool, its builder and the gate: a schema compiles once.
std::shared_ptr<minimal::node::SchemaCache> Schemas() {
    static const auto schemas = std::make_shared<minimal::node::SchemaCache>();
    return schemas;
}

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("node")

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::PluginRegistrationContext& context) {
    (void)context;  // configured per preset (tool_config.node), not in plugins.d
    registry.Register(minimal::node::MakeSubmitResultTool(Schemas()));
}

void minimal_plugin_register_preset_extensions(
    minimal::agent::PresetExtensionRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    (void)context;
    registry.AddDefinitionBuilder(
        minimal::node::kSubmitToolName,
        [schemas = Schemas()](const minimal::agent::PresetToolContext& preset) {
            return minimal::node::BuildSubmitResultDefinition(preset, *schemas);
        });
}

void minimal_plugin_register_turn_gates(minimal::agent::TurnGateRegistry& registry,
                                        const minimal::plugins::PluginRegistrationContext& context) {
    (void)context;
    registry.Register(minimal::node::MakeInputGate(Schemas()));
}

}  // extern "C"
