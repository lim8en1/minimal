#include "input_gate.hpp"

#include <string>
#include <utility>

#include "node_config.hpp"

namespace minimal::node {
namespace {

class InputGate final : public agent::TurnGate {
public:
    explicit InputGate(std::shared_ptr<SchemaCache> schemas) : schemas_(std::move(schemas)) {}

    std::optional<std::string> Check(const agent::TurnGateRequest& request) override {
        const nlohmann::json* slice = FindNodeSlice(request.preset.tool_config);
        if (slice == nullptr) return std::nullopt;
        // Throws for a malformed slice -- the registry fails the turn closed
        // with the message, though resolve-time validation catches it first.
        const NodeConfig config = ParseNodeConfig(*slice);
        if (!config.input_schema) return std::nullopt;

        const nlohmann::json input =
            nlohmann::json::parse(request.prompt, nullptr, /*allow_exceptions=*/false);
        if (input.is_discarded()) {
            return "input is not JSON; this agent takes a JSON document matching its "
                   "input schema";
        }
        const auto violations = schemas_->Get(*config.input_schema)->Validate(input);
        if (!violations.empty()) {
            return "input does not match the agent's input schema:\n" +
                   FormatViolations(violations);
        }
        return std::nullopt;
    }

private:
    std::shared_ptr<SchemaCache> schemas_;
};

}  // namespace

std::shared_ptr<agent::TurnGate> MakeInputGate(std::shared_ptr<SchemaCache> schemas) {
    return std::make_shared<InputGate>(std::move(schemas));
}

}  // namespace minimal::node
