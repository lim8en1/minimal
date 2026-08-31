#include "minimal/tools/aliased_tool.hpp"

#include <utility>

namespace minimal::tools {

AliasedTool::AliasedTool(std::shared_ptr<Tool> inner, std::string surface_name)
    : inner_(std::move(inner)), spec_(inner_->spec()) {
    spec_.name = std::move(surface_name);
    spec_.definition.name = spec_.name;
}

const ToolSpec& AliasedTool::spec() const { return spec_; }

ToolResult AliasedTool::execute(const nlohmann::json& input, ToolContext& ctx) {
    return inner_->execute(input, ctx);
}

bool AliasedTool::path_confinement_supported() const { return inner_->path_confinement_supported(); }

std::optional<std::string> AliasedTool::ExtractPath(const nlohmann::json& input) const {
    return inner_->ExtractPath(input);
}

std::optional<ToolCategory> AliasedTool::category_for(const nlohmann::json& input) const {
    return inner_->category_for(input);
}

PermissionDisclosure AliasedTool::Disclose(const nlohmann::json& input, const ToolContext& ctx) const {
    PermissionDisclosure disclosure = inner_->Disclose(input, ctx);
    // A tool that does not override Disclose inherits the base default: its own
    // name, no grants, complete. Through the wrapper that default would advertise
    // the ORIGINAL name in the Ask prompt while the model called it by its alias.
    // Recognise exactly that shape and restate it under the surface name; any
    // real override (custom summary or grants) passes through untouched.
    if (disclosure.grants.empty() && disclosure.complete && disclosure.summary == inner_->spec().name) {
        disclosure.summary = spec_.name;
    }
    return disclosure;
}

}  // namespace minimal::tools
