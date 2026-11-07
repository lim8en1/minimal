#include "submit_result_tool.hpp"

#include <stdexcept>
#include <string>
#include <utility>

#include "node_config.hpp"

namespace minimal::node {
namespace {

constexpr const char* kBaseDescription =
    "Submit the final result of this task. The input must match the required "
    "schema exactly. A valid submission ends the task -- call it once, when the "
    "result is complete. An invalid one returns the schema violations; fix them "
    "and call again.";

std::string Description(const std::string& extra) {
    return extra.empty() ? std::string(kBaseDescription)
                         : std::string(kBaseDescription) + "\n\n" + extra;
}

tools::ToolSpec MakeSpec() {
    tools::ToolSpec spec;
    spec.name = kSubmitToolName;
    // No side effect anywhere: the call's whole effect is ending the turn with
    // a value. ReadOnly is pre-allowed, which a headless node (ALLOW_ONLY, no
    // human to ask) needs -- an Ask would auto-deny the node's only exit.
    spec.category = tools::ToolCategory::ReadOnly;
    spec.definition.name = kSubmitToolName;
    spec.definition.description = Description("");
    // Placeholder: a preset without an output schema never sees the tool (the
    // builder withholds it), and one with a schema gets that schema instead.
    nlohmann::json schema = nlohmann::json::object();
    schema["type"] = "object";
    spec.definition.input_schema = std::move(schema);
    return spec;
}

class SubmitResultTool final : public tools::Tool {
public:
    explicit SubmitResultTool(std::shared_ptr<SchemaCache> schemas)
        : spec_(MakeSpec()), schemas_(std::move(schemas)) {}

    const tools::ToolSpec& spec() const override { return spec_; }
    std::string_view config_namespace() const override { return kNamespace; }

    tools::ToolResult execute(const nlohmann::json& input, tools::ToolContext& ctx) override {
        tools::ToolResult result;
        // The builder withholds the tool from presets without an output
        // schema, so reaching here without one means the advertised and the
        // active preset diverged (a mid-session /agent swap) -- refuse rather
        // than accept an unchecked result.
        if (ctx.tool_config == nullptr) {
            result.is_error = true;
            result.content_text = "submit_result is unavailable: this agent declares no "
                                  "tool_config.node.output_schema";
            return result;
        }
        NodeConfig config;
        std::shared_ptr<const CompiledSchema> schema;
        try {
            config = ParseNodeConfig(*ctx.tool_config);
            if (!config.output_schema) {
                throw std::invalid_argument("this agent declares no tool_config.node.output_schema");
            }
            schema = schemas_->Get(*config.output_schema);
        } catch (const std::exception& error) {
            result.is_error = true;
            result.content_text = std::string("submit_result is unavailable: ") + error.what();
            return result;
        }

        const auto violations = schema->Validate(input);
        if (!violations.empty()) {
            result.is_error = true;
            result.content_text =
                "The result does not match the required schema; nothing was submitted. "
                "Fix these and call submit_result again:\n" +
                FormatViolations(violations);
            return result;
        }
        result.content_text = "Result accepted.";
        result.turn_result = input;
        return result;
    }

private:
    tools::ToolSpec spec_;
    std::shared_ptr<SchemaCache> schemas_;
};

}  // namespace

std::unique_ptr<tools::Tool> MakeSubmitResultTool(std::shared_ptr<SchemaCache> schemas) {
    return std::make_unique<SubmitResultTool>(std::move(schemas));
}

llm::ToolDefinition BuildSubmitResultDefinition(const agent::PresetToolContext& context,
                                                SchemaCache& schemas) {
    llm::ToolDefinition withheld;  // empty name: the preset does not get the tool
    const nlohmann::json* slice = FindNodeSlice(context.preset.tool_config);
    if (slice == nullptr) return withheld;

    const auto fail = [&](const std::string& what) {
        return std::invalid_argument("agent preset '" + context.preset_key + "': " + what);
    };
    NodeConfig config;
    try {
        config = ParseNodeConfig(*slice);
    } catch (const std::exception& error) {
        throw fail(error.what());
    }
    // Both schemas compile HERE, at resolve time: a broken contract fails the
    // preset when it is selected, not on the first turn or the first submit.
    if (config.input_schema) {
        try {
            schemas.Get(*config.input_schema);
        } catch (const std::exception& error) {
            throw fail(std::string("tool_config.node.input_schema is not a valid JSON Schema: ") +
                       error.what());
        }
    }
    if (!config.output_schema) return withheld;
    try {
        schemas.Get(*config.output_schema);
    } catch (const std::exception& error) {
        throw fail(std::string("tool_config.node.output_schema is not a valid JSON Schema: ") +
                   error.what());
    }

    llm::ToolDefinition definition;
    definition.name = kSubmitToolName;
    definition.description = Description(config.description);
    definition.input_schema = *config.output_schema;
    // Constrained decoding where the provider can do it; submit_result's own
    // validation enforces the contract either way.
    definition.strict = IsStrictCompatible(*config.output_schema);
    return definition;
}

}  // namespace minimal::node
