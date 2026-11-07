#include "node_config.hpp"

#include <stdexcept>

namespace minimal::node {

NodeConfig ParseNodeConfig(const nlohmann::json& slice) {
    if (!slice.is_object()) {
        throw std::invalid_argument("tool_config.node must be a map");
    }
    NodeConfig config;
    for (const auto& [key, value] : slice.items()) {
        if (key == "input_schema" || key == "output_schema") {
            if (!value.is_object()) {
                throw std::invalid_argument("tool_config.node." + key +
                                            " must be a JSON Schema object");
            }
            (key == "input_schema" ? config.input_schema : config.output_schema) = value;
        } else if (key == "description") {
            if (!value.is_string()) {
                throw std::invalid_argument("tool_config.node.description must be a string");
            }
            config.description = value.get<std::string>();
        } else {
            throw std::invalid_argument("tool_config.node: unknown key '" + key +
                                        "' (want input_schema, output_schema, description)");
        }
    }
    return config;
}

const nlohmann::json* FindNodeSlice(const nlohmann::json& tool_config) {
    if (!tool_config.is_object()) return nullptr;
    const auto it = tool_config.find(kNamespace);
    return it == tool_config.end() ? nullptr : &it.value();
}

}  // namespace minimal::node
